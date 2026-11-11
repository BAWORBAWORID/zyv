#!/usr/bin/env bash
set -e

# ==============================================================================
# Zyvor Node Docker Entrypoint
# ==============================================================================
# Fitur Auto-Bore (Tunnel Relay):
# - ENABLE_BORE=1 atau ENABLE_BORE=true  : Menjalankan tunnel bore ke server publik.
# - BORE_PORT=<port>                     : Menentukan port publik bore yang diminta (opsional).
#                                          Jika tidak diisi, bore.pub akan memilihkan port acak.
# - BORE_SERVER=<host>                   : Server bore (default: bore.pub).
# - BORE_SECRET=<secret>                 : Secret token server bore jika diperlukan.
# ==============================================================================

if [ "${ENABLE_BORE:-0}" = "1" ] || [ "${ENABLE_BORE}" = "true" ] || [ "${USE_BORE:-0}" = "1" ] || [ -n "${BORE_PORT}" ]; then
    BORE_SERVER="${BORE_SERVER:-bore.pub}"
    BORE_ARGS="local 9933 --to ${BORE_SERVER}"

    # Validasi BORE_PORT: harus angka 1-65535. Nilai tidak valid (mis. dari platform)
    # diabaikan — bore.pub akan memilihkan port acak yang pasti tersedia.
    if [ -n "${BORE_PORT}" ] && [ "${BORE_PORT}" != "0" ]; then
        case "${BORE_PORT}" in
            ''|[!0-9]*) echo "[BORE] BORE_PORT tidak valid ('${BORE_PORT}') — pakai port acak";;
            *) if [ "${BORE_PORT}" -ge 1 ] && [ "${BORE_PORT}" -le 65535 ]; then
                   BORE_ARGS="${BORE_ARGS} --port ${BORE_PORT}"
               else
                   echo "[BORE] BORE_PORT ${BORE_PORT} di luar 1-65535 — pakai port acak"
               fi;;
        esac
    fi

    if [ -n "${BORE_SECRET}" ]; then
        BORE_ARGS="${BORE_ARGS} --secret ${BORE_SECRET}"
    fi

    export BORE_ON=1
    echo "[BORE] Menghubungkan tunnel ke ${BORE_SERVER}..."
    bore ${BORE_ARGS} > /tmp/bore.log 2>&1 &
    BORE_PID=$!

    ADV_ADDR=""
    for i in $(seq 1 30); do
        if grep -q "listening at" /tmp/bore.log 2>/dev/null; then
            ADV_ADDR=$(grep -o 'listening at [^ ]*' /tmp/bore.log | tail -n 1 | awk '{print $3}')
            break
        fi
        if ! kill -0 $BORE_PID 2>/dev/null; then
            echo "[BORE] Gagal tersambung ke server tunnel:"
            cat /tmp/bore.log
            break
        fi
        sleep 0.2
    done

    if [ -n "${ADV_ADDR}" ]; then
        export ZYVOR_EXTERNAL_ADDR="${ADV_ADDR}"
        echo "[BORE] Tunnel aktif! Alamat relay publik: ${ZYVOR_EXTERNAL_ADDR}"
    elif [ -n "${BORE_PORT}" ] && [ "${BORE_PORT}" != "0" ] && [ "${BORE_PORT}" -ge 1 ] 2>/dev/null && [ "${BORE_PORT}" -le 65535 ] 2>/dev/null; then
        export ZYVOR_EXTERNAL_ADDR="${BORE_SERVER}:${BORE_PORT}"
        echo "[BORE] Tunnel aktif pada port: ${ZYVOR_EXTERNAL_ADDR}"
    else
        echo "[BORE] Peringatan: Tidak dapat mendeteksi port tunnel dari log."
    fi
fi

# Pastikan direktori data ada
mkdir -p /data /root/.zyvor

# Node P2P harus listen 9933 saat bore aktif (bore mem-forward 9933)
if [ "${BORE_ON:-0}" = "1" ]; then
    NODE_PORT_ARGS="9933"
else
    NODE_PORT_ARGS=""
fi

# Jalankan perintah: jika tanpa argumen atau "node", sync dulu baru jalankan daemon node
if [ $# -eq 0 ]; then
    /usr/src/zyvor/zyvor sync || true
    if [ -n "$NODE_PORT_ARGS" ]; then
        exec stdbuf -oL /usr/src/zyvor/zyvor node "$NODE_PORT_ARGS"
    else
        exec stdbuf -oL /usr/src/zyvor/zyvor node
    fi
elif [ "$1" = "node" ]; then
    shift
    /usr/src/zyvor/zyvor sync || true
    if [ -n "$NODE_PORT_ARGS" ] && [ $# -eq 0 ]; then
        exec stdbuf -oL /usr/src/zyvor/zyvor node "$NODE_PORT_ARGS"
    else
        exec stdbuf -oL /usr/src/zyvor/zyvor node "$@"
    fi
else
    exec /usr/src/zyvor/zyvor "$@"
fi
