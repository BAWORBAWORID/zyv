FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# Dependensi build: g++ + OpenSSL dev + curl (unduh bore)
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ \
    make \
    libssl-dev \
    libcurl4-openssl-dev \
    ca-certificates \
    curl \
 && rm -rf /var/lib/apt/lists/*

# bore TIDAK diinstall lagi — node jalan lokal saja (tanpa publish tunnel publik).
# Kalau nanti butuh tunnel lagi, blok install bore ini bisa diaktifkan kembali:
# RUN set -eux; \
#     curl -fsSL https://github.com/ekzhang/bore/releases/download/v0.6.0/bore-v0.6.0-x86_64-unknown-linux-musl.tar.gz -o /tmp/bore.tgz; \
#     tar -xzf /tmp/bore.tgz -C /tmp; \
#     BIN=$(find /tmp -type f -name bore | head -1); \
#     mv "$BIN" /usr/local/bin/bore; \
#     chmod +x /usr/local/bin/bore; \
#     /usr/local/bin/bore --version

# Build dari source
WORKDIR /usr/src/zyvor
COPY src/ src/
COPY Makefile Makefile
COPY README.md README.md
RUN make

# Data node (chain + mempool). Wallet TIDAK di sini — milik pengguna (volume/ENV).
RUN mkdir -p /data /root/.zyvor
WORKDIR /data

# P2P 9933
EXPOSE 9933/tcp

ENV ZYVOR_RPC_PORT=8933
# Seed default sudah bore.pub:5315 (params.h). Override: -e ZYVOR_SEEDS=...

# Startup: node LOKAL saja (tanpa tunnel bore publik).
# Sync penuh dengan seed dulu (simpan rantai lokal di /data), baru node jalan.
CMD ["bash", "-c", "/usr/src/zyvor/zyvor sync; exec stdbuf -oL /usr/src/zyvor/zyvor node"]
