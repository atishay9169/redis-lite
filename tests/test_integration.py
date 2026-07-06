#!/usr/bin/env python3
"""
Integration test suite for redis-lite -- exercises the server as a real
client would, over actual TCP sockets. Complements tests/test_resp.cpp
(which tests the parser in isolation, no sockets/epoll involved).

Run against a live redis-lite instance:
    ./redis-lite-asan <port> &
    python3 test_integration.py <port>
"""
import socket
import sys
import time
import threading

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 7799
HOST = "127.0.0.1"

passed = 0
failed = 0


def ok(label, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
        print(f"PASS  {label}")
    else:
        failed += 1
        print(f"FAIL  {label}  {detail}")


def cmd(*args):
    out = f"*{len(args)}\r\n".encode()
    for a in args:
        b = a if isinstance(a, bytes) else str(a).encode()
        out += b"$" + str(len(b)).encode() + b"\r\n" + b + b"\r\n"
    return out


def connect(timeout=3):
    return socket.create_connection((HOST, PORT), timeout=timeout)


def recv_all(s, n, timeout=3):
    """Read exactly n bytes (or until timeout), for use against partial
    reads / slow-drain scenarios."""
    s.settimeout(timeout)
    buf = b""
    try:
        while len(buf) < n:
            chunk = s.recv(min(65536, n - len(buf)))
            if not chunk:
                break
            buf += chunk
    except socket.timeout:
        pass
    return buf


# ---------------------------------------------------------------------------
# 1. Basic protocol correctness (already covered manually; now automated)
# ---------------------------------------------------------------------------
def test_basic_protocol():
    s = connect()
    s.sendall(cmd("PING"))
    ok("PING -> PONG", s.recv(100) == b"+PONG\r\n")

    s.sendall(cmd("SET", "foo", "bar"))
    ok("SET -> OK", s.recv(100) == b"+OK\r\n")

    s.sendall(cmd("GET", "foo"))
    ok("GET hit", s.recv(100) == b"$3\r\nbar\r\n")

    s.sendall(cmd("GET", "nope"))
    ok("GET miss -> nil", s.recv(100) == b"$-1\r\n")

    s.sendall(cmd("DEL", "foo", "nope"))
    ok("DEL multi -> count", s.recv(100) == b":1\r\n")

    s.sendall(cmd("SET", "bin", b"ab\r\ncd"))
    s.recv(100)
    s.sendall(cmd("GET", "bin"))
    ok("binary-safe value round-trip", s.recv(100) == b"$6\r\nab\r\ncd\r\n")
    s.close()


# ---------------------------------------------------------------------------
# 2. Pipelining and split delivery over a real socket
# ---------------------------------------------------------------------------
def test_pipeline_and_split():
    s = connect()
    s.sendall(cmd("SET", "a", "1") + cmd("GET", "a") + cmd("PING"))
    time.sleep(0.1)
    ok("pipelined burst, 3 replies concatenated",
       s.recv(4096) == b"+OK\r\n$1\r\n1\r\n+PONG\r\n")

    half = cmd("GET", "a")
    s.sendall(half[:7])
    time.sleep(0.05)
    s.sendall(half[7:])
    ok("command split across two sends", s.recv(100) == b"$1\r\n1\r\n")
    s.close()


# ---------------------------------------------------------------------------
# 3. Errors: arity, unknown command, protocol error -> server closes
# ---------------------------------------------------------------------------
def test_errors_and_close():
    s = connect()
    s.sendall(cmd("GET"))
    ok("arity error", s.recv(200) ==
       b"-ERR wrong number of arguments for 'get' command\r\n")

    s.sendall(cmd("BLAH"))
    ok("unknown command error", s.recv(200) ==
       b"-ERR unknown command 'BLAH'\r\n")

    s.sendall(b"?bogus\r\n")
    reply = s.recv(200)
    ok("protocol error -> -ERR reply", reply.startswith(b"-ERR"))
    rest = s.recv(100)
    ok("server closes connection after protocol error", rest == b"")
    s.close()


# ---------------------------------------------------------------------------
# 4. Clean EOF teardown (client-initiated close, no error)
# ---------------------------------------------------------------------------
def test_eof_teardown():
    s = connect()
    s.sendall(cmd("PING"))
    s.recv(100)
    s.close()  # orderly close -- server should see EOF (read() == 0), reap it

    time.sleep(0.1)
    # Server should still be alive and accepting new connections after this.
    s2 = connect()
    s2.sendall(cmd("PING"))
    ok("server survives client EOF, still accepts new conns",
       s2.recv(100) == b"+PONG\r\n")
    s2.close()


# ---------------------------------------------------------------------------
# 5. Many concurrent connections (never tested beyond 1-2 clients before)
# ---------------------------------------------------------------------------
def test_many_concurrent_connections(n=500):
    socks = []
    try:
        for _ in range(n):
            socks.append(connect(timeout=5))
        for sock in socks:
            sock.sendall(cmd("PING"))
        results = []
        for sock in socks:
            results.append(sock.recv(100))
        expected = b"+PONG\r\n"
        bad_count = sum(1 for r in results if r != expected)
        ok(f"{n} concurrent connections all PONG",
           bad_count == 0,
           f"{bad_count} bad replies")

        # cross-check: each connection's writes are isolated (no crosstalk)
        for i, sock in enumerate(socks[:20]):
            sock.sendall(cmd("SET", f"k{i}", f"v{i}"))
        for i, sock in enumerate(socks[:20]):
            ok_reply = sock.recv(100) == b"+OK\r\n"
            if not ok_reply:
                ok(f"conn {i} isolated SET", False)
        for i, sock in enumerate(socks[:20]):
            sock.sendall(cmd("GET", f"k{i}"))
        mismatches = 0
        for i, sock in enumerate(socks[:20]):
            expect = f"${len(f'v{i}')}\r\nv{i}\r\n".encode()
            if sock.recv(100) != expect:
                mismatches += 1
        ok("no crosstalk between concurrent connections", mismatches == 0,
           f"{mismatches} mismatches")
    finally:
        for sock in socks:
            sock.close()


# ---------------------------------------------------------------------------
# 6. Real backpressure: force EAGAIN on write() and confirm EPOLLOUT
#    arm -> flush -> disarm actually recovers correctly, and that a slow
#    reader on one connection does NOT stall other connections.
# ---------------------------------------------------------------------------
def test_backpressure():
    # Build a payload big enough to overflow typical kernel socket buffers
    # (default SO_SNDBUF is often ~128-212KB) so write() is forced to
    # return partial/EAGAIN and the server must buffer + arm EPOLLOUT.
    big_val = b"x" * (1024 * 1024)  # 1 MB value

    setup = connect()
    setup.sendall(cmd("SET", "big", big_val))
    ok("backpressure setup: SET big value", setup.recv(100) == b"+OK\r\n")
    setup.close()

    slow = connect(timeout=10)
    # Pipeline enough GETs that the total reply size overflows the send
    # buffer many times over -- guarantees try_flush() hits EAGAIN at
    # least once and must arm EPOLLOUT.
    n_gets = 40
    slow.sendall(cmd("GET", "big") * n_gets)

    # Deliberately do NOT read yet. While this client is stalled, prove a
    # second, independent client is still served immediately -- this is
    # the actual point of backpressure being per-connection.
    other = connect(timeout=3)
    other.sendall(cmd("PING"))
    t0 = time.time()
    other_reply = other.recv(100)
    elapsed = time.time() - t0
    ok("other client unaffected by stalled slow client",
       other_reply == b"+PONG\r\n" and elapsed < 1.0,
       f"reply={other_reply!r} elapsed={elapsed:.3f}s")
    other.close()

    # Now drain the slow client and verify every byte arrived correctly --
    # this is the real test that arm -> flush -> disarm didn't drop or
    # corrupt data on the EAGAIN path.
    expected_one = b"$" + str(len(big_val)).encode() + b"\r\n" + big_val + b"\r\n"
    expected_total = expected_one * n_gets
    got = recv_all(slow, len(expected_total), timeout=15)
    ok("slow client eventually receives all bytes, byte-exact",
       got == expected_total,
       f"got {len(got)} of {len(expected_total)} expected bytes")
    slow.close()

    # Confirm EPOLLOUT was actually disarmed afterward: a brand new client
    # must still get instant service (if disarm failed, the loop would be
    # busy-spinning but *should* still serve -- so this is a soft check;
    # the real proof is server responsiveness, tested next).
    check = connect()
    check.sendall(cmd("PING"))
    ok("server responsive after backpressure episode ends",
       check.recv(100) == b"+PONG\r\n")
    check.close()


# ---------------------------------------------------------------------------
# 7. Unbounded input buffer (the flagged-but-unimplemented hardening TODO)
# ---------------------------------------------------------------------------
def test_unbounded_input_not_capped():
    # Send a huge, NEVER-terminated bulk header ($999999999\r\n...) --
    # under the current code this should buffer forever, unbounded. This
    # test documents that gap rather than pretending it's handled: it
    # SUCCEEDS (bug reproduced) if the server accepts an arbitrarily large
    # partial command without closing/erroring within a short window.
    s = connect(timeout=3)
    garbage = b"*1\r\n$1000000000\r\n" + b"a" * (2 * 1024 * 1024)  # 2MB, no end
    s.sendall(garbage)
    s.settimeout(1.5)
    try:
        reply = s.recv(100)
    except socket.timeout:
        reply = b""  # no error, no close -- buffering silently, as expected
    still_open = reply == b""
    if still_open:
        print("INFO  unbounded-input gap confirmed: server buffered 2MB "
              "of a never-completing command with no error/close "
              "(TODO(hardening) in EventLoop.cpp is not yet implemented)")
    else:
        print(f"INFO  server responded {reply!r} -- hardening may already "
              f"be partially in place")
    s.close()

if __name__ == "__main__":
    print(f"--- redis-lite integration suite (port {PORT}) ---\n")
    test_basic_protocol()
    test_pipeline_and_split()
    test_errors_and_close()
    test_eof_teardown()
    test_many_concurrent_connections()
    test_backpressure()
    test_unbounded_input_not_capped()

    print(f"\n--- {passed} passed, {failed} failed ---")
    sys.exit(1 if failed else 0)
