# Multi-stage Dockerfile for hft-engine
# Build stage
FROM debian:bookworm AS builder
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake git ca-certificates curl && \
    rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . /src
# Configure & build (Release)
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel
# Runtime stage
FROM debian:bookworm-slim AS runtime
ENV DBN_FILE=/data/input.dbn \
    PORT=8080 \
    LATENCY_P99_WARN_NS=10000000
RUN useradd -u 10001 engine && mkdir -p /data && chown engine:engine /data
WORKDIR /app
COPY --from=builder /src/build/hft-engine /app/hft-engine
USER engine
EXPOSE 8080
# Healthcheck: expect metrics endpoint to return 200 and contain total_messages
HEALTHCHECK --interval=30s --timeout=3s --retries=3 CMD curl -fs http://localhost:${PORT}/metrics | grep -q 'total_messages' || exit 1
CMD ["/app/hft-engine"]
