FROM debian:bookworm-slim AS builder

WORKDIR /src

RUN apt-get update \
    && apt-get install -y --no-install-recommends cmake g++ make \
    && rm -rf /var/lib/apt/lists/*

COPY . .

RUN cmake -S . -B build \
    && cmake --build build

FROM debian:bookworm-slim AS runtime

WORKDIR /app

RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates redis-tools \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/redis-lite /app/redis-lite

ENV PORT=6380
EXPOSE 6380

ENTRYPOINT ["/bin/sh", "-c", "/app/redis-lite ${PORT}"]
