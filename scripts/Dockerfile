# Cross-compilation environment for forgetful (target: Ableton Move, aarch64 Linux)
FROM debian:bookworm

RUN apt-get update && apt-get install -y \
    gcc-aarch64-linux-gnu \
    libc6-dev-arm64-cross \
    && rm -rf /var/lib/apt/lists/*

ENV CROSS_PREFIX=aarch64-linux-gnu-

WORKDIR /build
