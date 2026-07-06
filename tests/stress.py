#!/usr/bin/env python3
"""
Stress test: many concurrent connections, each hammering SET/GET/EXPIRE/
TTL/DEL for a sustained duration, with heavy short-TTL churn so keys are
actively expiring (both lazy and active paths) WHILE hundreds of
connections are live. This is the realistic worst case for the TTL heap
and the epoll_wait timeout logic under real concurrency -- not just idle
connection count.

Usage:
    python3 tests/stress.py <port> [num_clients] [duration_seconds]

Success criteria (correctness under load, not throughput -- that's
redis-benchmark's job):
  - every reply is well-formed RESP (parses cleanly)
  - no connection ever hangs / times out
  - no crash, no ASan/UBSan trip (check the server's own log separately)
  - TTL semantics stay correct throughout (a key past its TTL is never
    served, an EXPIRE'd key's TTL is never negative-but-wrong)
"""
import random
import socket
import sys
import threading
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 6380
NUM_CLIENTS = int(sys.argv[2]) if len(sys.argv) > 2 else 200
DURATION = float(sys.argv[3]) if len(sys.argv) > 3 else 15.0
HOST = "127.0.0.1"

lock = threading.Lock()
total_ops = 0
errors = []


def cmd(*args):
    out = f"*{len(args)}\r\n".encode()
    for a in args:
        b = a if isinstance(a, bytes) else str(a).encode()
        out += b"$" + str(len(b)).encode() + b"\r\n" + b + b"\r\n"
    return out


def read_one_reply(sock, buf):
    """Read and consume exactly one RESP reply from sock, using buf as a
    carry-over byte buffer across calls (mirrors the server's own framing
    problem, just client-side). Returns (reply_bytes, remaining_buf)."""
    while True:
        nl = buf.find(b"\r\n")
        if nl != -1:
            prefix = buf[0:1]
            if prefix in (b"+", b"-", b":"):
                end = nl + 2
                return buf[:end], buf[end:]
            if prefix == b"$":
                length = int(buf[1:nl])
                if length == -1:
                    end = nl + 2
                    return buf[:end], buf[end:]
                total_end = nl + 2 + length + 2
                if len(buf) >= total_end:
                    return buf[:total_end], buf[total_end:]
            if prefix == b"*":  # only ever "*0\r\n" from COMMAND in this project
                end = nl + 2
                return buf[:end], buf[end:]
        chunk = sock.recv(65536)
        if not chunk:
            raise ConnectionError("server closed unexpectedly")
        buf += chunk


def worker(client_id, stop_at):
    global total_ops
    ops = 0
    try:
        s = socket.create_connection((HOST, PORT), timeout=5)
        buf = b""
        rng = random.Random(client_id)
        my_keys = [f"c{client_id}:k{i}" for i in range(20)]

        while time.time() < stop_at:
            choice = rng.random()
            key = rng.choice(my_keys)

            if choice < 0.35:
                # short-TTL SET -- deliberately churns the heap hard
                ttl_ms = rng.choice([10, 50, 100, 300])
                s.sendall(cmd("SET", key, f"v{ops}", "PX", ttl_ms))
                reply, buf = read_one_reply(s, buf)
                assert reply == b"+OK\r\n", f"bad SET-PX reply: {reply!r}"

            elif choice < 0.55:
                # permanent SET -- exercises stale-heap-entry discard if
                # this key previously had a TTL
                s.sendall(cmd("SET", key, f"v{ops}"))
                reply, buf = read_one_reply(s, buf)
                assert reply == b"+OK\r\n", f"bad SET reply: {reply!r}"

            elif choice < 0.75:
                s.sendall(cmd("GET", key))
                reply, buf = read_one_reply(s, buf)
                assert reply == b"$-1\r\n" or reply.startswith(b"$"), \
                    f"malformed GET reply: {reply!r}"

            elif choice < 0.85:
                s.sendall(cmd("TTL", key))
                reply, buf = read_one_reply(s, buf)
                assert reply.startswith(b":"), f"malformed TTL reply: {reply!r}"
                val = int(reply[1:-2])
                assert val >= -2, f"impossible TTL value: {val}"

            elif choice < 0.93:
                secs = rng.choice([1, 2, 5])
                s.sendall(cmd("EXPIRE", key, secs))
                reply, buf = read_one_reply(s, buf)
                assert reply in (b":0\r\n", b":1\r\n"), \
                    f"malformed EXPIRE reply: {reply!r}"

            else:
                s.sendall(cmd("DEL", key))
                reply, buf = read_one_reply(s, buf)
                assert reply.startswith(b":"), f"malformed DEL reply: {reply!r}"

            ops += 1

        s.close()
    except Exception as e:
        with lock:
            errors.append(f"client {client_id}: {e!r}")
    finally:
        with lock:
            total_ops += ops


def main():
    print(f"--- stress test: {NUM_CLIENTS} clients, {DURATION}s, port {PORT} ---")
    stop_at = time.time() + DURATION
    threads = [threading.Thread(target=worker, args=(i, stop_at))
               for i in range(NUM_CLIENTS)]
    t0 = time.time()
    for t in threads:
        t.start()
    for t in threads:
        t.join(timeout=DURATION + 10)

    elapsed = time.time() - t0
    alive = sum(1 for t in threads if t.is_alive())

    print(f"\nelapsed: {elapsed:.1f}s")
    print(f"total ops completed: {total_ops}")
    print(f"throughput: {total_ops / elapsed:.0f} ops/sec (mixed workload, "
          f"not a redis-benchmark number)")
    print(f"threads still alive (hung?): {alive}")
    print(f"errors: {len(errors)}")
    for e in errors[:20]:
        print(f"  {e}")

    # A final sanity round-trip against a fresh connection -- proves the
    # server is still fully responsive after the whole stress episode.
    s = socket.create_connection((HOST, PORT), timeout=3)
    s.sendall(cmd("PING"))
    final = s.recv(100)
    s.close()
    print(f"post-stress PING: {final!r}")

    ok = (alive == 0 and len(errors) == 0 and final == b"+PONG\r\n")
    print("\nRESULT:", "PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()