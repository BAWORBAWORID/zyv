#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <chrono>
#include <thread>
#include <sstream>
#include <ctime>
#include <filesystem>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

#include "params.h"
#include "block.h"
#include "blockchain.h"
#include "mempool.h"
#include <map>
#include "keys.h"
#include "sha256.h"
#include "p2p.h"
#include "miner.h"
#include <atomic>
static std::atomic<bool> mdStop{false};
#include <csignal>
#include "rpc.h"

using namespace zyv;

static const std::string CHAIN_FILE = "zyvor_chain.dat";
static const std::string MEMPOOL_FILE = "zyvor_mempool.dat";
// Wallet TIDAK disimpan di direktori node/jaringan — milik pengguna saja,
// di home directory: ~/.zyvor/wallet.dat
static std::string walletFilePath() {
    const char* home = std::getenv("HOME");
    std::string dir = (home ? std::string(home) : ".") + "/.zyvor";
    std::filesystem::create_directories(dir);
    return dir + "/wallet.dat";
}
static const std::string WALLET_FILE = walletFilePath();

static const uint64_t TX_FEE = MIN_TX_FEE; // fee jaringan (zyvor per tx)

// ------------------------------------------------------------------ persistence
static void loadChain(Blockchain& bc) {
    std::ifstream f(CHAIN_FILE);
    if (!f.good()) return;
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.empty()) return;
    bc.deserialize(data);
    bc.validateChain(); // rebuild UTXO
}
static void saveChain(const Blockchain& bc) {
    std::ofstream f(CHAIN_FILE, std::ios::trunc);
    f << bc.serialize();
}
static void loadMempool(Mempool& mp) {
    std::ifstream f(MEMPOOL_FILE);
    if (!f.good()) return;
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (!data.empty()) mp.deserialize(data);
}
static void saveMempool(const Mempool& mp) {
    std::ofstream f(MEMPOOL_FILE, std::ios::trunc);
    f << mp.serialize();
}
// Ambil pubkey alamat dari node lokal RPC (/api/pubkey?addr=X) jika chain lokal CLI belum catchup
static std::string fetchPubkeyRpc(const std::string& addr) {
    uint16_t rpcPort = 8933;
    if (const char* env = std::getenv("ZYVOR_RPC_PORT")) {
        try { rpcPort = uint16_t(std::stoi(env)); } catch (...) {}
    }
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return "";
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(rpcPort);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, (sockaddr*)&a, sizeof(a)) < 0) { ::close(fd); return ""; }
    std::string req = "GET /api/pubkey?addr=" + addr + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    ::send(fd, req.data(), req.size(), MSG_NOSIGNAL);
    char buf[1024];
    std::string resp;
    for (;;) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        resp.append(buf, buf + n);
        if (resp.size() > 4096) break;
    }
    ::close(fd);
    size_t pos = resp.find("\"pubkey\":\"");
    if (pos == std::string::npos) return "";
    pos += 10;
    size_t end = resp.find("\"", pos);
    if (end == std::string::npos) return "";
    return resp.substr(pos, end - pos);
}
// Gagal = node lokal tidak jalan → tx tetap di mempool file (akan tersebar saat node mine).
static bool broadcastTxRpc(const Transaction& tx) {
    uint16_t rpcPort = 8933;
    if (const char* env = std::getenv("ZYVOR_RPC_PORT")) {
        try { rpcPort = uint16_t(std::stoi(env)); } catch (...) {}
    }
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(rpcPort);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0) { ::close(fd); return false; }
    std::string body = tx.serialize();
    std::string req = "POST /api/sendrawtx HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                      "Content-Type: text/plain\r\nContent-Length: "
                      + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    ssize_t sent = 0;
    while (sent < (ssize_t)req.size()) {
        ssize_t n = ::send(fd, req.data() + sent, req.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) { ::close(fd); return false; }
        sent += n;
    }
    char buf[512];
    std::string resp;
    for (;;) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        resp.append(buf, buf + n);
        if (resp.size() > 4096) break;
    }
    ::close(fd);
    return resp.find("\"ok\"") != std::string::npos;
}
// ------------------------------------------------------------------ wallet storage
// Passphrase dari env ZYVOR_WALLET_PASS. Bila diset, wallet disimpan
// terenkripsi (AES-256-GCM); tanpa env, fallback plaintext (mode lama).
static std::string walletPass() {
    const char* p = std::getenv("ZYVOR_WALLET_PASS");
    return p ? std::string(p) : std::string();
}

static std::vector<KeyPair> loadWallet() {
    std::vector<KeyPair> w;
    std::ifstream f(WALLET_FILE);
    if (!f.good()) return w;
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    std::string body = data;
    std::string pass = walletPass();
    if (data.rfind("ZYVW1|", 0) == 0) {
        if (pass.empty()) {
            std::cerr << "[wallet] wallet terenkripsi — set ZYVOR_WALLET_PASS\n";
            return w;
        }
    }
    if (!pass.empty()) {
        if (!decryptWalletFile(WALLET_FILE, body, pass)) {
            std::cerr << "[wallet] gagal dekripsi (passphrase salah?) — wallet tidak dimuat\n";
            return w;
        }
    }
    std::istringstream iss(body);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.empty()) continue;
        try {
            w.push_back(keyPairFromPrivate(line));
        } catch (...) {}
    }
    return w;
}
static void saveWalletAll(const std::vector<KeyPair>& w) {
    std::string body;
    for (const auto& k : w) body += k.privateKey + "\n";
    std::string pass = walletPass();
    if (!pass.empty()) {
        if (!encryptWalletFile(WALLET_FILE, body, pass)) {
            std::cerr << "[wallet] gagal enkripsi wallet!\n";
        }
    } else {
        std::ofstream f(WALLET_FILE, std::ios::trunc);
        f << body;
    }
}
static void addKeyPair(const KeyPair& kp) {
    auto w = loadWallet();
    w.push_back(kp);
    saveWalletAll(w);
}

// ------------------------------------------------------------------ helpers
static uint64_t parseAmount(const std::string& s) {
    size_t dot = s.find('.');
    std::string whole = (dot == std::string::npos) ? s : s.substr(0, dot);
    std::string frac = (dot == std::string::npos) ? "" : s.substr(dot + 1);
    while (frac.size() < 8) frac += '0';
    frac = frac.substr(0, 8);
    uint64_t w = whole.empty() ? 0 : std::stoull(whole);
    uint64_t fr = frac.empty() ? 0 : std::stoull(frac);
    return w * SATOSHIS_PER_COIN + fr;
}

static void printInfo(const Blockchain& bc) {
    std::cout << "====================================================\n";
    std::cout << " " << COIN_NAME << " (" << TICKER << ") " << SYMBOL << "\n";
    std::cout << "====================================================\n";
    std::cout << " Pencipta        : " << CREATOR << "\n";
    std::cout << " Konsensus       : Proof-of-Work (SHA-256)\n";
    std::cout << " Kriptografi     : ECDSA secp256k1 + RIPEMD160 + Base58Check\n";
    std::cout << " Block time      : " << TARGET_BLOCK_TIME_SEC / 60 << " menit\n";
    std::cout << " Reward awal     : 50 " << TICKER << "/blok (halving tiap 4 tahun)\n";
    std::cout << " Max supply      : " << MAX_SUPPLY << " " << TICKER << "\n";
    std::cout << " Unit terkecil   : 1 " << TICKER << " = " << SATOSHIS_PER_COIN << " zyvor\n";
    std::cout << " Koin terakhir   : ~" << LAST_COIN_YEAR << "\n\n";
    std::cout << " Tinggi rantai   : " << bc.height() << " blok\n";
    std::cout << " Kesulitan       : " << bc.difficulty() << " bit nol\n";
    std::cout << " Halving         : " << bc.halvingCount(bc.height() - 1) << "x\n";
    std::cout << " UTXO            : " << bc.utxo().size() << " output\n";
    std::cout << " Sudah di-mine   : " << formatCoin(bc.totalMined()) << " " << TICKER << "\n";
    std::cout << " Sisa            : " << formatCoin(bc.remaining()) << " " << TICKER << "\n";
    std::cout << " Reward sekarang : " << formatCoin(bc.rewardForHeight(bc.height())) << " " << TICKER << "/blok\n";
}

static void printBlock(const Block& b) {
    std::cout << "----------------------------------------------------\n";
    std::cout << " Blok #" << b.index << "  hash: " << b.hash << "\n";
    std::cout << "  prevHash : " << b.prevHash << "\n";
    std::cout << "  merkle   : " << b.merkleRoot << "\n";
    std::cout << "  nonce    : " << b.nonce << "  diff " << b.difficulty << " bit\n";
    std::cout << "  txs      : " << b.transactions.size() << "\n";
    for (const auto& t : b.transactions) {
        if (t.isCoinbase()) {
            std::cout << "    [COINBASE] -> " << t.outputs[0].address
                      << " : " << formatCoin(t.outputs[0].amount) << " " << TICKER << "\n";
        } else {
            std::cout << "    " << t.id.substr(0, 16) << "... "
                      << t.inputs.size() << " in -> " << t.outputs.size() << " out ("
                      << formatCoin(t.totalOutput()) << ")\n";
        }
    }
}

static void usage() {
    std::cout <<
        "Zyvor Coin (ZYVC) v0.0.1 — node inti (C++)\n\n"
        "Penggunaan:\n"
        "  zyvor info                    Info koin + supply\n"
        "  zyvor chain                   Tampilkan seluruh rantai\n"
        "  zyvor validate                Validasi integritas rantai\n"
        "  zyvor keygen                  Buat alamat baru (ECDSA)\n"
        "  zyvor wallet                  Daftar alamat + saldo\n"
        "  zyvor addr [privkey]          Alamat+pubkey dari wallet.dat / derive privkey\n"
        "  zyvor verify <privkey> <addr> Cek privkey cocok dengan alamat\n"
        "  zyvor balance <alamat>        Saldo sebuah alamat\n"
        "  zyvor send <dari> <ke> <amt>  Kirim ZYVC (mis. 10.5)\n"
        "  zyvor sendmsg <ke> <pesan>    Kirim pesan chat (auto-encrypt E2E; --plain utk terbuka)\n"
        "  zyvor register                Daftarkan pubkey alamat pertama agar bisa terima pesan E2E\n"
        "  zyvor mempool                 Daftar transaksi menunggu\n"
        "  zyvor mine <N> [miner]        Tambang N blok (dengan mempool)\n"
        "  zyvor miner solo [alamat] [interval_s] [always]\n"
        "                                Miner daemon (poll mempool, auto-mine)\n"
        "  zyvor miner pool <url> <worker> <payout_addr>\n"
        "                                Worker stratum: share ke pool, PPLNS payout\n"
        "  zyvor node [port]             Jalankan node (default: port acak; isi port utk menerima peer)\n"
        "  zyvor connect <ip:port> ...   Sambungkan ke peer\n"
        "  zyvor hash <teks>             SHA-256\n"
        "  zyvor reset                   Hapus semua data lokal\n";
}

int main(int argc, char** argv) {
    std::string cmd = (argc > 1) ? argv[1] : "info";

    if (cmd == "hash" && argc > 2) { std::cout << sha256(argv[2]) << "\n"; return 0; }
    if (cmd == "help" || cmd == "--help" || cmd == "-h") { usage(); return 0; }

    if (cmd == "reset") {
        std::remove(CHAIN_FILE.c_str());
        std::remove(MEMPOOL_FILE.c_str());
        std::remove(WALLET_FILE.c_str());
        std::cout << "Data lokal dihapus.\n";
        return 0;
    }

    Blockchain bc;
    Mempool mp;
    loadChain(bc);
    loadMempool(mp);
    if (bc.height() == 0) bc.createGenesis();

    // ---------------------------------------------------------------- sync (CLI)
    // Sinkronisasi sekali jalan dengan server utama, lalu simpan rantai lokal.
    // Dipakai eksplisit ("zyvor sync") dan otomatis sebelum wallet/balance.
    auto autoSync = [&](bool verbose) {
        size_t total = 0;
        for (int round = 0; round < 8; round++) { // maks 8x50 = 400 blok per sesi
            int n = P2PNode::syncOneShot(DEFAULT_SEED_NODE, bc);
            if (n <= 0) break;
            total += (size_t)n;
        }
        if (total > 0) {
            saveChain(bc);
            if (verbose)
                std::cout << "[SYNC] " << total << " blok baru diterima dari "
                          << DEFAULT_SEED_NODE << " (height=" << bc.height() << ")\n";
        } else if (verbose) {
            std::cout << "[SYNC] sudah terbaru (height=" << bc.height() << ")\n";
        }
        return total;
    };

    if (cmd == "sync") {
        std::cout << "Sinkronisasi dengan " << DEFAULT_SEED_NODE << "...\n";
        autoSync(true);
        std::cout << "Height lokal: " << bc.height() << "\n";
        return 0;
    }

    // ---------------------------------------------------------------- keygen
    if (cmd == "keygen") {
        KeyPair kp = generateKeyPair();
        addKeyPair(kp);
        std::cout << "Alamat baru:\n";
        std::cout << "  address : " << kp.address << "\n";
        std::cout << "  privkey : " << kp.privateKey << "\n";
        std::cout << "  pubkey  : " << kp.publicKey << "\n";
        return 0;
    }

    // ---------------------------------------------------------------- wallet
    if (cmd == "wallet") {
        autoSync(false); // biar saldo tidak tertinggal dari server utama
        auto w = loadWallet();
        if (w.empty()) { std::cout << "Belum ada alamat. Jalankan: zyvor keygen\n"; return 0; }
        for (size_t i = 0; i < w.size(); i++) {
            std::cout << "[" << i << "] " << w[i].address
                      << "  saldo " << formatCoin(bc.balanceOf(w[i].address)) << " " << TICKER << "\n";
        }
        return 0;
    }

    // ---------------------------------------------------------------- addr (derivation-only, tanpa jaringan)
    // Tampilkan alamat + public key dari wallet.dat ATAU dari privkey yang ditempel.
    // Tidak menyentuh chain — cocok untuk verifikasi kunci offline.
    if (cmd == "addr") {
        auto w = loadWallet();
        if (argc > 2) {
            // mode: zyvor addr <privkey_hex>  → derive tanpa menyimpan
            std::string priv = argv[2];
            KeyPair kp = keyPairFromPrivate(priv);
            if (kp.address.empty()) { std::cout << "Privkey tidak valid (harus hex 64 char).\n"; return 1; }
            std::cout << "privkey : " << kp.privateKey << "\n";
            std::cout << "pubkey  : " << kp.publicKey << "\n";
            std::cout << "address : " << kp.address << "\n";
            return 0;
        }
        if (w.empty()) { std::cout << "Belum ada alamat. Jalankan: zyvor keygen\n"; return 0; }
        for (size_t i = 0; i < w.size(); i++) {
            std::cout << "[" << i << "] address : " << w[i].address << "\n";
            std::cout << "    pubkey  : " << w[i].publicKey << "\n";
        }
        return 0;
    }

    // ---------------------------------------------------------------- verify (privkey cocok dengan alamat?)
    if (cmd == "verify" && argc > 3) {
        std::string priv = argv[2], wantAddr = argv[3];
        KeyPair kp = keyPairFromPrivate(priv);
        if (kp.address.empty()) { std::cout << "Privkey tidak valid.\n"; return 1; }
        if (kp.address == wantAddr) {
            std::cout << "VALID: privkey = alamat " << wantAddr << "\n";
            return 0;
        }
        std::cout << "TIDAK COCOK: privkey ini menghasilkan alamat " << kp.address << "\n";
        return 1;
    }

    // ---------------------------------------------------------------- balance
    if (cmd == "balance" && argc > 2) {
        autoSync(false); // saldo selalu segar dari server utama
        std::cout << formatCoin(bc.balanceOf(argv[2])) << " " << TICKER << "\n";
        return 0;
    }

    // ---------------------------------------------------------------- send
    if (cmd == "send" && argc > 4) {
        std::string from = argv[2], to = argv[3];
        uint64_t amount = parseAmount(argv[4]);
        // arg ke-6 opsional: data/memo (maks 80 byte, disimpan hex on-chain)
        std::string memo;
        bool plain = false;
        if (argc > 5) {
            for (int i = 5; i < argc; i++) {
                std::string a = argv[i];
                if (a == "--plain") { plain = true; continue; }
                if (!memo.empty()) memo += " ";
                memo += a;
            }
            if (memo.size() > MAX_TX_DATA_BYTES) {
                std::cout << "Data/memo terlalu besar (maks " << MAX_TX_DATA_BYTES
                          << " byte).\n";
                return 1;
            }
        }

        // Default: Enkripsi E2E (ECIES) jika ada memo & bukan --plain
        if (!memo.empty() && !plain) {
            std::map<std::string, std::string> idx;
            for (const auto& b : bc.chain()) {
                for (const auto& t : b.transactions) {
                    if (t.inputs.empty() || t.inputs[0].pubkey.empty()) continue;
                    std::string f = publicKeyToAddress(t.inputs[0].pubkey);
                    if (!idx.count(f)) idx[f] = t.inputs[0].pubkey;
                }
            }
            std::string targetPub;
            auto it = idx.find(to);
            if (it != idx.end()) {
                targetPub = it->second;
            } else {
                targetPub = fetchPubkeyRpc(to);
            }
            if (targetPub.empty()) {
                std::cout << "Peringatan: Pubkey penerima belum dikenal on-chain. Gunakan --plain jika ingin mengirim memo terbuka.\n";
                return 1;
            }
            std::string ct = encryptMessage(targetPub, memo);
            if (ct.empty()) {
                std::cout << "Enkripsi memo gagal.\n";
                return 1;
            }
            memo = ct;
        }

        auto w = loadWallet();
        KeyPair* kp = nullptr;
        for (auto& k : w) if (k.address == from) { kp = &k; break; }
        if (!kp) { std::cout << "Alamat pengirim tidak ada di wallet.\n"; return 1; }

        auto utxos = bc.utxo().allForAddress(from);
        uint64_t fee = TX_FEE;
        // fee data: di atas DATA_FREE_LIMIT, +DATA_FEE_PER_KB per 1000 byte (pembulatan ke atas)
        if (memo.size() > DATA_FREE_LIMIT) {
            size_t extra = memo.size() - DATA_FREE_LIMIT;
            uint64_t units = (extra + 999) / 1000;
            fee += units * DATA_FEE_PER_KB;
        }
        uint64_t need = amount + fee;
        uint64_t total = 0;
        std::vector<OutPoint> selected;
        for (const auto& kv : utxos) {
            selected.push_back(kv.first);
            total += kv.second.amount;
            if (total >= need) break;
        }
        if (total < need) {
            std::cout << "Saldo kurang: butuh " << formatCoin(need)
                      << ", tersedia " << formatCoin(total) << "\n";
            return 1;
        }

        Transaction tx;
        for (const auto& op : selected) {
            TxIn in;
            in.prevOut = op;
            in.pubkey = kp->publicKey;
            tx.inputs.push_back(in);
        }
        tx.outputs.push_back({amount, to});
        uint64_t change = total - amount - TX_FEE;
        if (change > 0) tx.outputs.push_back({change, from});
        tx.timestamp = uint64_t(time(nullptr));
        if (!memo.empty()) tx.data = memo;
        tx.id = computeTxId(tx);
        for (auto& in : tx.inputs) in.signature = signMessage(kp->privateKey, tx.id);

        if (!bc.validateTx(tx)) { std::cout << "Transaksi gagal validasi.\n"; return 1; }
        mp.add(tx);
        saveMempool(mp);

        std::cout << "Transaksi dibuat (masuk mempool):\n";
        std::cout << "  txid   : " << tx.id << "\n";
        std::cout << "  kirim  : " << formatCoin(amount) << " " << TICKER << " -> " << to << "\n";
        if (change > 0) std::cout << "  kembalian: " << formatCoin(change) << " " << TICKER << " -> " << from << "\n";
        std::cout << "  fee    : " << formatCoin(fee) << " " << TICKER << "\n";
        if (broadcastTxRpc(tx))
            std::cout << "  broadcast: ✓ (tx tersebar ke jaringan)\n";
        else
            std::cout << "  broadcast: - (node lokal tidak jalan; tx tersebar saat node mine)\n";
        std::cout << "Tambang blok agar transaksi terkonfirmasi: zyvor mine 1\n";
        return 0;
    }

    // ---------------------------------------------------------------- register (daftarkan pubkey ke chain agar bisa menerima pesan terenkripsi)
    if (cmd == "register") {
        auto w = loadWallet();
        if (w.empty()) { std::cout << "Wallet kosong. Jalankan: zyvor keygen\n"; return 1; }
        KeyPair* kp = &w[0];
        std::string me = kp->address;
        uint64_t amount = parseAmount("0.0001");
        uint64_t fee = TX_FEE;
        auto utxos = bc.utxo().allForAddress(me);
        uint64_t total = 0;
        std::vector<OutPoint> selected;
        for (const auto& kv : utxos) {
            selected.push_back(kv.first);
            total += kv.second.amount;
            if (total >= amount + fee) break;
        }
        if (total < amount + fee) {
            std::cout << "Saldo kurang untuk register (butuh " << formatCoin(amount + fee) << ").\n";
            return 1;
        }
        Transaction tx;
        for (const auto& op : selected) {
            TxIn in; in.prevOut = op; in.pubkey = kp->publicKey;
            tx.inputs.push_back(in);
        }
        tx.outputs.push_back({amount, me});
        uint64_t change = total - amount - fee;
        if (change > 0) tx.outputs.push_back({change, me});
        tx.timestamp = uint64_t(time(nullptr));
        tx.id = computeTxId(tx);
        for (auto& in : tx.inputs) in.signature = signMessage(kp->privateKey, tx.id);
        if (!bc.validateTx(tx)) { std::cout << "Transaksi gagal validasi.\n"; return 1; }
        mp.add(tx);
        saveMempool(mp);
        if (broadcastTxRpc(tx))
            std::cout << "Register ✓ — tx tersebar ke jaringan. Setelah masuk blok, alamat ini\n"
                      << "bisa menerima pesan terenkripsi: " << me << "\n";
        else
            std::cout << "Register ✓ (mempool lokal) — node lokal tidak jalan, tersebar saat mine.\n"
                      << "Alamat: " << me << "\n";
        return 0;
    }

    // ---------------------------------------------------------------- sendmsg (chat CLI: auto-encrypt, pesan bebas)
    if (cmd == "sendmsg" && argc > 3) {
        std::string to = argv[2];
        std::string msg;
        bool plain = false;
        for (int i = 3; i < argc; i++) {
            std::string a = argv[i];
            if (a == "--plain") { plain = true; continue; }
            if (!msg.empty()) msg += " ";
            msg += a;
        }
        if (msg.empty()) { std::cout << "Pesan kosong.\n"; return 1; }
        if (msg.size() > MAX_TX_DATA_BYTES) {
            std::cout << "Pesan terlalu besar (maks " << MAX_TX_DATA_BYTES << " byte).\n";
            return 1;
        }

        // pengirim: alamat pertama di wallet
        auto w = loadWallet();
        if (w.empty()) { std::cout << "Wallet kosong. Jalankan: zyvor keygen\n"; return 1; }
        KeyPair* kp = &w[0];
        std::string from = kp->address;

        // enkripsi E2E kecuali --plain (butuh pubkey penerima dari index on-chain)
        if (!plain) {
            std::map<std::string, std::string> idx;
            for (const auto& b : bc.chain())
                for (const auto& t : b.transactions) {
                    if (t.inputs.empty() || t.inputs[0].pubkey.empty()) continue;
                    std::string f = publicKeyToAddress(t.inputs[0].pubkey);
                    if (!idx.count(f)) idx[f] = t.inputs[0].pubkey;
                }
            auto it = idx.find(to);
            if (it == idx.end()) {
                std::cout << "Tidak bisa enkripsi: pubkey penerima belum dikenal (dia belum pernah kirim tx).\n";
                std::cout << "Kirim pesan terbuka: zyvor sendmsg " << to << " <pesan> --plain\n";
                return 1;
            }
            std::string ct = encryptMessage(it->second, msg);
            if (ct.empty()) { std::cout << "Enkripsi gagal.\n"; return 1; }
            msg = ct;
        }

        // jumlah kirim: minimal 1 zyvor-unit (0.000001) + fee — pakai 0.0001 ala chat
        uint64_t amount = parseAmount("0.0001");
        auto utxos = bc.utxo().allForAddress(from);
        uint64_t fee = TX_FEE;
        if (msg.size() > DATA_FREE_LIMIT) {
            size_t extra = msg.size() - DATA_FREE_LIMIT;
            uint64_t units = (extra + 999) / 1000;
            fee += units * DATA_FEE_PER_KB;
        }
        uint64_t need = amount + fee;
        uint64_t total = 0;
        std::vector<OutPoint> selected;
        for (const auto& kv : utxos) {
            selected.push_back(kv.first);
            total += kv.second.amount;
            if (total >= need) break;
        }
        if (total < need) {
            std::cout << "Saldo kurang: butuh " << formatCoin(need) << ", tersedia " << formatCoin(total) << "\n";
            return 1;
        }

        Transaction tx;
        for (const auto& op : selected) {
            TxIn in; in.prevOut = op; in.pubkey = kp->publicKey;
            tx.inputs.push_back(in);
        }
        tx.outputs.push_back({amount, to});
        uint64_t change = total - amount - fee;
        if (change > 0) tx.outputs.push_back({change, from});
        tx.timestamp = uint64_t(time(nullptr));
        tx.data = msg;
        tx.id = computeTxId(tx);
        for (auto& in : tx.inputs) in.signature = signMessage(kp->privateKey, tx.id);

        if (!bc.validateTx(tx)) { std::cout << "Transaksi gagal validasi.\n"; return 1; }
        mp.add(tx);
        saveMempool(mp);

        std::cout << (plain ? "Pesan terbuka" : "Pesan terenkripsi (E2E)")
                  << " terkirim ke mempool:\n";
        std::cout << "  dari  : " << from << "\n";
        std::cout << "  ke    : " << to << "\n";
        std::cout << "  txid  : " << tx.id << "\n";
        std::cout << "  fee   : " << formatCoin(fee) << " " << TICKER << "\n";
        if (broadcastTxRpc(tx))
            std::cout << "  broadcast: ✓ (pesan tersebar real-time ke seluruh jaringan)\n";
        else
            std::cout << "  broadcast: - (node lokal tidak jalan; pesan tersebar saat node mine)\n";
        std::cout << "Penerima lihat instan via: zyvor inbox " << to << "\n";
        return 0;
    }

    // ---------------------------------------------------------------- mempool
    if (cmd == "mempool") {
        if (mp.size() == 0) { std::cout << "Mempool kosong.\n"; return 0; }
        for (const auto& t : mp.all()) {
            std::cout << t.id << "  " << t.inputs.size() << " in -> "
                      << t.outputs.size() << " out (" << formatCoin(t.totalOutput()) << ")\n";
        }
        return 0;
    }

    // ---------------------------------------------------------------- inbox (baca pesan masuk on-chain)
    if (cmd == "inbox" && argc > 2) {
        std::string me = argv[2];
        auto w = loadWallet();
        bool mine = false;
        for (const auto& k : w) if (k.address == me) { mine = true; break; }
        if (!mine) std::cout << "(catatan: alamat tidak ada di wallet ini - tetap dibaca)\n";

        // kumpulkan semua tx ber-data dari rantai (terbaru dulu)
        struct Msg { std::string from, data; int block; uint64_t ts; };
        std::vector<Msg> in;
        const auto& chain = bc.chain();
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            for (const auto& t : it->transactions) {
                if (t.data.empty() || t.isCoinbase()) continue;
                bool toMe = false;
                for (const auto& o : t.outputs) if (o.address == me) { toMe = true; break; }
                if (!toMe) continue;
                std::string from = t.inputs.empty() ? "?" : publicKeyToAddress(t.inputs[0].pubkey);
                in.push_back({from, t.data, (int)it->index, t.timestamp});
            }
        }
        // pesan PENDING (masih di mempool, belum masuk blok) — diterima real-time via P2P
        Mempool mpl; loadMempool(mpl);
        std::vector<std::pair<Transaction,std::string>> pending;
        for (const auto& t : mpl.all()) {
            if (t.data.empty() || t.isCoinbase()) continue;
            bool toMe = false;
            for (const auto& o : t.outputs) if (o.address == me) { toMe = true; break; }
            if (!toMe) continue;
            std::string from = t.inputs.empty() ? "?" : publicKeyToAddress(t.inputs[0].pubkey);
            pending.push_back({t, from});
        }
        if (in.empty() && pending.empty()) { std::cout << "Inbox kosong.\n"; return 0; }
        std::cout << "Inbox " << me << " (" << (in.size()+pending.size()) << " pesan):\n";
        for (const auto& [t, from] : pending) {
            std::time_t tt = (std::time_t)t.timestamp;
            char buf[32];
            std::strftime(buf, sizeof(buf), "%d-%m-%Y %H:%M", std::localtime(&tt));
            std::cout << "  PENDING (mem pool)  " << buf << "  dari " << from << "\n";
            if (isEncryptedMemo(t.data)) {
                std::string shown;
                if (mine) {
                    std::string privMe;
                    for (const auto& k : w) if (k.address == me) { privMe = k.privateKey; break; }
                    std::string err;
                    std::string pt = decryptMessage(privMe, t.data, err);
                    shown = pt.empty() ? ("[terenkripsi - " + err + "]") : pt;
                } else {
                    shown = "[terenkripsi - butuh wallet penerima]";
                }
                std::cout << "    > " << shown << "\n";
            } else {
                std::cout << "    > " << t.data << "\n";
            }
        }
        for (const auto& m : in) {
            std::time_t tt = (std::time_t)m.ts;
            char buf[32];
            std::strftime(buf, sizeof(buf), "%d-%m-%Y %H:%M", std::localtime(&tt));
            std::cout << "  blok " << m.block << "  " << buf << "  dari " << m.from << "\n";
            if (isEncryptedMemo(m.data)) {
                // dekrip pakai private key alamat ini (kalau ada di wallet)
                std::string shown;
                if (mine) {
                    std::string privMe;
                    for (const auto& k : w) if (k.address == me) { privMe = k.privateKey; break; }
                    std::string err;
                    std::string pt = decryptMessage(privMe, m.data, err);
                    shown = pt.empty() ? ("[terenkripsi - " + err + "]") : pt;
                } else {
                    shown = "[terenkripsi - butuh wallet penerima]";
                }
                std::cout << "    > " << shown << "\n";
            } else {
                std::cout << "    > " << m.data << "\n";
            }
        }
        return 0;
    }

    // ---------------------------------------------------------------- mine
    if (cmd == "mine") {
        long long n = 1;
        std::string miner;
        if (argc > 2) n = std::stoll(argv[2]);
        if (argc > 3) miner = argv[3];

        for (long long i = 0; i < n; i++) {
            auto start = std::chrono::steady_clock::now();
            const Block& b = bc.mineBlock(mp.all(), miner);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();

            std::cout << "Mined blok #" << b.index << "  nonce=" << b.nonce
                      << "  txs=" << b.transactions.size() << "  (" << ms << " ms)\n";

            // keluarkan tx yang sudah masuk blok dari mempool
            std::vector<std::string> included;
            for (size_t t = 1; t < b.transactions.size(); t++) {
                included.push_back(b.transactions[t].id);
            }
            mp.remove(included);
        }
        // buang tx stale (input sudah terpakai tx lain / UTXO hilang)
        {
            std::vector<Transaction> pool = mp.all();
            size_t stale = bc.purgeMempool(pool);
            if (stale > 0) {
                mp.clear();
                for (const auto& t : pool) mp.add(t);
                std::cout << "Mempool: " << stale << " tx stale dibuang\n";
            }
        }
        saveChain(bc);
        saveMempool(mp);
        std::cout << "Rantai disimpan (" << CHAIN_FILE << "), tinggi " << bc.height() << "\n";
        return 0;
    }

    // ---------------------------------------------------------------- chain
    if (cmd == "chain") {
        for (const auto& b : bc.chain()) printBlock(b);
        return 0;
    }

    // ---------------------------------------------------------------- validate
    if (cmd == "validate") {
        bool ok = bc.validateChain();
        std::cout << (ok ? "OK: rantai valid." : "GAGAL: rantai tidak valid!") << "\n";
        return ok ? 0 : 1;
    }

    // ---------------------------------------------------------------- node (P2P + RPC daemon)
    if (cmd == "node") {
        // Default: port acak (client-mode ramah HP/PC).
        // Custom: ./zyvor node <port>  ATAU  env ZYVOR_P2P_PORT=<port>
        uint16_t p2pPort = 0;
        if (argc > 2) p2pPort = uint16_t(std::stoi(argv[2]));
        else if (const char* env = std::getenv("ZYVOR_P2P_PORT")) p2pPort = uint16_t(std::stoi(env));

        // Auto-sync SEBELUM node jalan (sama seperti `zyvor sync`) — rantai lokal
        // langsung terbaru, jaringan saling terhubung tanpa langkah manual.
        {
            std::cout << "Sinkronisasi dengan " << DEFAULT_SEED_NODE << "...\n";
            int n = P2PNode::syncOneShot(DEFAULT_SEED_NODE, bc);
            if (n > 0) { saveChain(bc); saveMempool(mp); }
            std::cout << "Height lokal: " << bc.height() << "\n";
        }

        P2PNode p2p(bc, mp);
        RpcServer rpc(bc, mp, p2p);
        if (!p2p.start(p2pPort)) {
            std::cout << "[P2P] port " << p2pPort << " terpakai — lanjut dengan port acak (client-mode)\n";
            p2p.start(0); // node tetap berfungsi penuh sebagai klien
        }
        uint16_t rpcPort = 8933;
        if (const char* env = std::getenv("ZYVOR_RPC_PORT")) rpcPort = uint16_t(std::stoi(env));
        if (!rpc.start(rpcPort)) {
            rpcPort = 20000 + uint16_t(std::rand() % 20000); // langsung port acak
            if (!rpc.start(rpcPort)) {
                std::cout << "[RPC] tetap gagal — lanjut TANPA RPC (P2P & mining tetap jalan)\n";
                rpcPort = 0;
            } else {
                std::cout << "[RPC] fallback ke port acak " << rpcPort << "\n";
            }
        }

        std::cout << "Node Zyvor jalan. Ctrl+C untuk berhenti.\n";
        std::cout << "  P2P : " << p2p.port() << "   RPC: http://127.0.0.1:" << rpcPort << "\n";
        // sambungkan ke peer awal: argumen > env ZYVOR_SEEDS > DNS seed default
        std::vector<std::string> seeds;
        for (int i = 3; i < argc; i++) seeds.push_back(argv[i]);
        if (const char* env = std::getenv("ZYVOR_SEEDS")) {
            std::stringstream ss(env);
            std::string s;
            while (std::getline(ss, s, ',')) if (!s.empty()) seeds.push_back(s);
        }

        // Ambil peers dari DNS seed node.zyvor.my.id bila tersedia di DNS
        auto dnsSeeds = P2PNode::resolveDnsSeed("node.zyvor.my.id", DEFAULT_SEED_PORT);
        if (!dnsSeeds.empty()) {
            std::cout << "[P2P] Ditemukan " << dnsSeeds.size() << " peer dari DNS seed node.zyvor.my.id\n";
            for (const auto& dp : dnsSeeds) seeds.push_back(dp);
        }

        if (seeds.empty()) seeds.push_back(DEFAULT_SEED_NODE); // bore.pub:5315

        // Sambungkan ke seeds
        for (const auto& s : seeds) {
            p2p.connectTo(s);
        }

        // Auto-mine setup
        std::string minerAddr;
        if (const char* env = std::getenv("ZYVOR_MINER_ADDR")) {
            minerAddr = env;
        } else {
            auto w = loadWallet();
            if (!w.empty()) {
                minerAddr = w[0].address;
            } else {
                KeyPair kp = generateKeyPair();
                minerAddr = kp.address;
                saveWalletAll({kp});
            }
        }

        bool autoMine = true;
        if (const char* env = std::getenv("ZYVOR_AUTOMINE")) {
            if (std::string(env) == "0" || std::string(env) == "false") autoMine = false;
        }
        int mineInterval = 60;
        if (const char* env = std::getenv("ZYVOR_MINE_INTERVAL")) {
            try { mineInterval = std::stoi(env); } catch (...) {}
        }
        if (mineInterval < 5) mineInterval = 5;

        if (autoMine) {
            std::cout << "[AutoMine] Aktif - interval " << mineInterval << "s, reward ke " << minerAddr << "\n";
        }

        auto lastMineTime = std::chrono::steady_clock::now();
        auto firstPendingTime = std::chrono::steady_clock::now();
        bool hasPending = false;

        // simpan rantai berkala (10 dtk, biar CLI inbox offline lihat pesan pending cepat)
        // + reconnect otomatis ke seeds + auto-mine jika mempool ada tx
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            saveChain(bc);
            saveMempool(mp);
            if (p2p.peers().empty()) {
                std::cout << "[P2P] tidak ada peer - mencoba reconnect seed...\n";
                for (const auto& s : seeds) p2p.connectTo(s);
            }

            if (autoMine) {
                if (mp.size() == 0) {
                    hasPending = false;
                } else {
                    auto now = std::chrono::steady_clock::now();
                    if (!hasPending) {
                        hasPending = true;
                        firstPendingTime = now;
                    }
                    auto pendingElapsed = std::chrono::duration_cast<std::chrono::seconds>(now - firstPendingTime).count();
                    auto sinceLastMine = std::chrono::duration_cast<std::chrono::seconds>(now - lastMineTime).count();
                    if (pendingElapsed >= mineInterval && sinceLastMine >= mineInterval) {
                        std::cout << "[AutoMine] Mempool ada " << mp.size() << " tx, menambang 1 blok...\n";
                        if (rpc.autoMine(minerAddr)) {
                            saveChain(bc);
                            saveMempool(mp);
                            lastMineTime = std::chrono::steady_clock::now();
                            hasPending = (mp.size() > 0);
                            if (hasPending) firstPendingTime = lastMineTime;
                            std::cout << "[AutoMine] Blok #" << bc.height() << " sukses ditambang & di-broadcast. Sisa mempool: " << mp.size() << "\n";
                        }
                    }
                }
            }
        }
    }

    // ---------------------------------------------------------------- miner daemon
    if (cmd == "miner" && argc > 2) {
        std::string mode = argv[2]; // solo | pool
        MinerDaemon md;
        std::thread t;
        if (mode == "solo") {
            std::string addr = (argc > 3) ? argv[3] : "";
            int interval = (argc > 4) ? std::stoi(argv[4]) : 60;
            bool always = (argc > 5 && std::string(argv[5]) == "always");
            t = std::thread([&, addr, interval, always] { md.runSolo(bc, mp, addr, interval, always, mdStop); });
        } else if (mode == "pool") {
            std::string pool = (argc > 3) ? argv[3] : "http://127.0.0.1:8933";
            std::string wname = (argc > 4) ? argv[4] : "worker1";
            std::string payout = (argc > 5) ? argv[5] : "";
            t = std::thread([&, pool, wname, payout] { md.runPoolWorker(pool, wname, payout, mdStop); });
        } else {
            std::cout << "gunakan: zyvor miner solo [alamat] [interval_s] [always]\n"
                         "        zyvor miner pool <url> <worker> <payout_addr>\n";
            return 1;
        }
        t.join();
        return 0;
    }

    // ---------------------------------------------------------------- connect
    if (cmd == "connect" && argc > 2) {
        P2PNode p2p(bc, mp);
        p2p.start(0); // port acak (client-only)
        for (int i = 2; i < argc; i++) p2p.connectTo(argv[i]);
        std::this_thread::sleep_for(std::chrono::seconds(3));
        std::cout << "Peers: ";
        for (const auto& p : p2p.peers()) std::cout << p << " ";
        std::cout << "\n";
        return 0;
    }

    // ---------------------------------------------------------------- default: info
    printInfo(bc);
    return 0;
}
