#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <chrono>
#include <thread>
#include <sstream>
#include <ctime>
#include <filesystem>

#include "params.h"
#include "block.h"
#include "blockchain.h"
#include "mempool.h"
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
        "  zyvor balance <alamat>        Saldo sebuah alamat\n"
        "  zyvor send <dari> <ke> <amt>  Kirim ZYVC (mis. 10.5)\n"
        "  zyvor mempool                 Daftar transaksi menunggu\n"
        "  zyvor mine <N> [miner]        Tambang N blok (dengan mempool)\n"
        "  zyvor miner solo [alamat] [interval_s] [always]\n"
        "                                Miner daemon (poll mempool, auto-mine)\n"
        "  zyvor miner pool <url> <worker> <payout_addr>\n"
        "                                Worker stratum: share ke pool, PPLNS payout\n"
        "  zyvor node [p2p_port]         Jalankan node P2P + RPC (default 9933)\n"
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
        auto w = loadWallet();
        if (w.empty()) { std::cout << "Belum ada alamat. Jalankan: zyvor keygen\n"; return 0; }
        for (size_t i = 0; i < w.size(); i++) {
            std::cout << "[" << i << "] " << w[i].address
                      << "  saldo " << formatCoin(bc.balanceOf(w[i].address)) << " " << TICKER << "\n";
        }
        return 0;
    }

    // ---------------------------------------------------------------- balance
    if (cmd == "balance" && argc > 2) {
        std::cout << formatCoin(bc.balanceOf(argv[2])) << " " << TICKER << "\n";
        return 0;
    }

    // ---------------------------------------------------------------- send
    if (cmd == "send" && argc > 4) {
        std::string from = argv[2], to = argv[3];
        uint64_t amount = parseAmount(argv[4]);
        // arg ke-6 opsional: data/memo (maks 80 byte, disimpan hex on-chain)
        std::string memo;
        if (argc > 5) {
            memo = argv[5];
            if (memo.size() > MAX_TX_DATA_BYTES) {
                std::cout << "Data/memo terlalu besar (maks " << MAX_TX_DATA_BYTES
                          << " byte).\n";
                return 1;
            }
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
        std::cout << "Tambang blok agar transaksi terkonfirmasi: zyvor mine 1\n";
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
        if (in.empty()) { std::cout << "Inbox kosong.\n"; return 0; }
        std::cout << "Inbox " << me << " (" << in.size() << " pesan):\n";
        for (const auto& m : in) {
            std::time_t tt = (std::time_t)m.ts;
            char buf[32];
            std::strftime(buf, sizeof(buf), "%d-%m-%Y %H:%M", std::localtime(&tt));
            std::cout << "  blok " << m.block << "  " << buf << "  dari " << m.from << "\n";
            std::cout << "    > " << m.data << "\n";
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
        uint16_t p2pPort = (argc > 2) ? uint16_t(std::stoi(argv[2])) : 9933;

        P2PNode p2p(bc, mp);
        RpcServer rpc(bc, mp, p2p);
        if (!p2p.start(p2pPort)) { std::cout << "Gagal bind P2P port " << p2pPort << "\n"; return 1; }
        uint16_t rpcPort = 8933;
        if (const char* env = std::getenv("ZYVOR_RPC_PORT")) rpcPort = uint16_t(std::stoi(env));
        if (!rpc.start(rpcPort)) { std::cout << "Gagal bind RPC port " << rpcPort << "\n"; return 1; }

        std::cout << "Node Zyvor jalan. Ctrl+C untuk berhenti.\n";
        std::cout << "  P2P : " << p2pPort << "   RPC: http://127.0.0.1:" << rpcPort << "\n";
        // sambungkan ke peer awal: argumen > env ZYVOR_SEEDS > DNS seed default
        std::vector<std::string> seeds;
        for (int i = 3; i < argc; i++) seeds.push_back(argv[i]);
        if (const char* env = std::getenv("ZYVOR_SEEDS")) {
            std::stringstream ss(env);
            std::string s;
            while (std::getline(ss, s, ',')) if (!s.empty()) seeds.push_back(s);
        }
        if (seeds.empty()) seeds.push_back(DEFAULT_SEED_NODE); // node.zyvor.my.id:9933
        // simpan rantai berkala (blok yang diterima via P2P ikut persist ke disk)
        // + reconnect otomatis ke seed kalau semua koneksi hilang
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            saveChain(bc);
            saveMempool(mp);
            if (p2p.peers().empty()) {
                std::cout << "[P2P] tidak ada peer - mencoba reconnect seed...\n";
                for (const auto& s : seeds) p2p.connectTo(s);
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
