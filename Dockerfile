FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# Dependensi build: g++ + OpenSSL dev + curl (retry bila mirror gagal)
RUN apt-get update \
 && (apt-get install -y --no-install-recommends g++ make libssl-dev libcurl4-openssl-dev ca-certificates curl \
 || (apt-get update && apt-get install -y --no-install-recommends g++ make libssl-dev libcurl4-openssl-dev ca-certificates curl)) \
 && rm -rf /var/lib/apt/lists/*

# Install bore binary untuk opsi relay tunnel
RUN set -eux; \
    curl -fsSL https://github.com/ekzhang/bore/releases/download/v0.6.0/bore-v0.6.0-x86_64-unknown-linux-musl.tar.gz -o /tmp/bore.tgz; \
    tar -xzf /tmp/bore.tgz -C /usr/local/bin bore; \
    chmod +x /usr/local/bin/bore; \
    rm -f /tmp/bore.tgz; \
    bore --version

# Build dari source
WORKDIR /usr/src/zyvor
COPY src/ src/
COPY Makefile Makefile
COPY README.md README.md
RUN make

# Data node (chain + mempool). Wallet milik pengguna (volume/ENV).
RUN mkdir -p /data /root/.zyvor
WORKDIR /data

# P2P 9933, RPC 8933
EXPOSE 9933/tcp 8933/tcp

ENV ZYVOR_RPC_PORT=8933
# Seed default sudah bore.pub:5315 (params.h). Override: -e ZYVOR_SEEDS=...

COPY entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
