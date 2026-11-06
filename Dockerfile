FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# Dependensi build: g++ + OpenSSL dev
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ \
    make \
    libssl-dev \
    ca-certificates \
 && rm -rf /var/lib/apt/lists/*

# Build dari source
WORKDIR /usr/src/zyvor
COPY src/ src/
COPY Makefile Makefile
COPY README.md README.md
RUN make

# Data node (chain + mempool). Wallet TIDAK di sini — milik pengguna (volume/ENV).
RUN mkdir -p /data /root/.zyvor
WORKDIR /data

# P2P 9933, RPC 8933 (RPC bind loopback di dalam container)
EXPOSE 9933/tcp

# Node jalan sebagai seed-connected full node.
# Override saat run: -e ZYVOR_SEEDS=... / ZYVOR_RPC_PORT=... / ZYVOR_WALLET_PASS=...
ENV ZYVOR_RPC_PORT=8933
CMD ["/usr/src/zyvor/zyvor", "node"]
