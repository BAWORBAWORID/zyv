# Zyvor Coin (ZYVC)

Implementasi inti blockchain ala Bitcoin dalam **C++ murni** — edukatif, tanpa dependensi berat.

## Spesifikasi

| Item            | Nilai                                     |
|-----------------|-------------------------------------------|
| Nama            | Zyvor Coin                                |
| Ticker          | ZYVC (Ƶ)                                  |
| Pencipta        | Zyvor                                     |
| Konsensus       | Proof-of-Work (SHA-256)                   |
| Kriptografi     | ECDSA secp256k1 + RIPEMD160 + Base58Check |
| Block time      | 10 menit                                  |
| Max supply      | 21.000.000 ZYVC                           |
| Unit terkecil   | 1 ZYVC = 100.000.000 zyvor                |
| Reward awal     | 50 ZYVC / blok                            |
| Halving         | tiap 210.000 blok (± 4 tahun)             |
| Difficulty adj. | tiap 2.016 blok (± 2 minggu)              |
| Fee minimum     | 0.0001 ZYVC per transaksi                 |
| Launch          | 1 Januari 2027 (genesis)                  |
| Koin terakhir   | ± 2158 (launch + 131 tahun)               |

## Fitur

1. **UTXO model** — output terkunci ke alamat, dihabiskan via input `txid:index`; double-spend dicegah (dalam tx, dalam blok, dan lintas blok).
2. **Wallet ECDSA** — keypair secp256k1 (OpenSSL), alamat Base58Check prefix `0x1C`, tiap input ditandatangani DER lalu diverifikasi.
3. **Mempool** — antrian tx valid; verifikasi penuh sebelum diterima; dibersihkan setelah terkonfirmasi.
4. **P2P antar node** — TCP line-based, POSIX socket + pthread. Handshake `VERSION/VERACK`, sinkronisasi blok `GETBLOCKS/BLOCK`, broadcast `NEWBLOCK/TX`, penemuan peer `GETPEERS/PEERS`.
5. **Fee jaringan** — minimum 0.0001 ZYVC; fee masuk ke coinbase miner (`coinbase = reward + Σfee`), divalidasi ketat saat blok diverifikasi.
6. **RPC API + explorer** — HTTP JSON di `127.0.0.1:8933` (loopback saja).

## Build

```bash
make            # butuh g++ 13+, OpenSSL dev headers
./zyvor info
```

## CLI

```
zyvor info                    Info koin + supply
zyvor chain                   Tampilkan seluruh rantai
zyvor validate                Validasi integritas rantai
zyvor keygen                  Buat alamat baru (ECDSA)
zyvor wallet                  Daftar alamat + saldo
zyvor balance <alamat>        Saldo sebuah alamat
zyvor send <dari> <ke> <amt>  Kirim ZYVC (fee 0.0001)
zyvor mempool                 Daftar transaksi menunggu
zyvor mine <N> [miner]        Tambang N blok
zyvor node [p2p_port] [peer..] Jalankan node P2P + RPC (default P2P 9933, RPC 8933)
zyvor connect <ip:port> ...   Sambung ke peer (one-shot)
zyvor hash <teks>             SHA-256
zyvor reset                   Hapus data lokal
```

Variabel lingkungan: `ZYVOR_RPC_PORT` untuk mengubah port RPC (berguna saat menjalankan >1 node di satu mesin).

## Menjalankan 2 node (contoh terverifikasi)

```bash
# terminal 1
cd /tmp/nodeA && ZYVOR_RPC_PORT=8933 ~/zyv-project/zyvor node 9933

# terminal 2
cd /tmp/nodeB && ZYVOR_RPC_PORT=8934 ~/zyv-project/zyvor node 9934 127.0.0.1:9933

# mine di node B -> node A otomatis menerima blok lewat P2P
curl "http://127.0.0.1:8934/api/mine?addr=<alamat>&n=1"
curl  http://127.0.0.1:8933/api/info     # height ikut naik
```

## RPC API (explorer)

Semua response JSON. Server bind ke `127.0.0.1` (tidak terekspos publik).

| Endpoint | Keterangan |
|---|---|
| `GET /` atau `/api/info` | Info node, tinggi rantai, supply, mempool, peer |
| `GET /api/block/<height>` | Detail blok + daftar txid |
| `GET /api/tx/<txid>` | Detail transaksi (atau status `mempool`) |
| `GET /api/address/<addr>` | Saldo + daftar UTXO |
| `GET /api/mempool` | Isi mempool |
| `GET /api/peers` | Daftar peer P2P |
| `POST /api/sendrawtx` | Body = serialisasi transaksi; validasi + relay ke peer |
| `GET /api/messages?addr=X` | Baca pesan (tx ber-data) per alamat; `&dir=in|out|both&limit=N`; status confirmed/pending |
| `POST /api/send?from=X&to=Y&amount=Z` | Kirim dari wallet node; memo opsional via `&data=` atau body POST |
| `GET /api/pubkey?addr=X` | Pubkey on-chain milik alamat (dari tx yang pernah dikirim) |
| `enc=1` pada /api/send | Enkripsi memo end-to-end (ECIES secp256k1 + AES-256-GCM); hanya penerima bisa baca via `zyvor inbox` |
| `GET /api/mine?addr=X&n=1` | Tambang N blok (maks 100), broadcast ke peer |

Contoh:

```bash
curl -s http://127.0.0.1:8933/api/info
curl -s http://127.0.0.1:8933/api/block/3
curl -s http://127.0.0.1:8933/api/address/CeFj1qTbDNmBLTqrh19ofU2KHRQbouHykn
curl -s "http://127.0.0.1:8933/api/mine?addr=<alamat>&n=1"
```

## Struktur

```
src/
  sha256.h/.cpp       SHA-256 dari nol
  params.h            konstanta jaringan (supply, halving, fee, genesis)
  transaction.h/.cpp  model transaksi UTXO + serialisasi
  keys.h/.cpp         ECDSA secp256k1, alamat Base58Check, tanda tangan
  utxo.h              UTXO set
  mempool.h           antrian transaksi
  block.h/.cpp        blok, merkle root, PoW, serialisasi
  blockchain.h/.cpp   rantai, mining, halving, difficulty, validasi, fee
  p2p.h/.cpp          jaringan P2P (POSIX socket + pthread)
  rpc.h/.cpp          HTTP JSON API + explorer
  main.cpp            CLI node
```

## Catatan keamanan (jujur)

- Wallet disimpan **plaintext** di `zyvor_wallet.dat` — hanya untuk belajar, jangan pakai dana nyata.
- RPC hanya bind loopback; tidak ada autentikasi. Jangan diubah ke `0.0.0.0` tanpa auth + firewall.
- Difficulty awal 4 bit (demo) — blok selesai dalam milidetik; sangat mudah di-51%. Naikkan `INITIAL_DIFFICULTY_BITS` sebelum dipakai serius.
- Belum ada: enkripsi wallet via prompt interaktif (pakai env `ZYVOR_WALLET_PASS`), checkpoint, SPV.
- Proyek **edukatif**, bukan uang elektronik siap produksi.

## Keamanan tambahan (Bagian 5–6)

### Fork resolution (rantai total-work terbesar)
- Blok dari jaringan yang tidak menyambung ke tip → **orphan pool** (maks 500).
- Fork yang menyambung ke blok tengah → dibandingkan **total work kumulatif** (`Σ 2^difficulty`);
  hanya cabang dengan work **lebih besar** yang menggantikan rantai aktif (reorg).
- Reorg memvalidasi ulang seluruh cabang alternatif + rebuild UTXO sebelum commit.
- Terverifikasi unit test: fork 1 blok (work sama) ditolak; fork 3 blok (work lebih besar) menang → tip berganti.

### Menyebarkan node (seed `node.zyvor.my.id:9933`)

- DNS `node.zyvor.my.id` → 119.28.71.87 (server utama).
- Node utama jalan 24/7 via systemd: `zyvor-node.service` (P2P 9933, RPC 8933).
- Node baru otomatis bootstrap ke seed: cukup `zyvor node` (tanpa argumen) —
  urutan seed: argumen CLI > env `ZYVOR_SEEDS` (dipisah koma) > `node.zyvor.my.id:9933`.
- Alternatif bila port 9933 diblokir (Security Group): tunnel bore permanen
  `bore-p2p.service` → `bore.pub:5315`; sambung dengan `zyvor connect bore.pub:5315`
  atau env `ZYVOR_SEEDS=bore.pub:5315`.

### Enkripsi wallet (AES-256-GCM + PBKDF2)
- Set `ZYVOR_WALLET_PASS=<passphrase>` → wallet otomatis tersimpan terenkripsi
  (`ZYVW1|` + salt 16B + IV 12B + ciphertext + GCM tag; PBKDF2 200.000 iterasi).
- Tanpa env: fallback plaintext (mode lama). File terenkripsi tanpa env → ditolak dengan pesan.
- Passphrase salah → dekripsi gagal (GCM tag), wallet tidak dimuat.

## Bukti uji (dijalankan di mesin ini)

```
# 2 node, sync + broadcast blok
height n1: 5   height n2: 5      (setelah handshake, sama)
mine 1 blok di n2 via RPC
height n1: 6                     (blok diterima via NEWBLOCK)

# fee jaringan
coinbase blok = 50.00010000 ZYVC = reward 50 + fee 0.0001  ✓
wallet: A 60.00000000, B 40.00000000 (kembalian + fee benar)  ✓
validate: OK: rantai valid  ✓
```
