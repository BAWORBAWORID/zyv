#include "rpc.h"
#include "sha256.h"
#include "block.h"
#include <fstream>
#include "keys.h"
#include "params.h"

#include <arpa/inet.h>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sstream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <ctime>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <algorithm>

namespace zyv {

// ---- util JSON ----
static std::string jstr(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out + "\"";
}

static std::string jsonOk(const std::string& inner) {
    return "{\"status\":\"ok\"," + inner.substr(inner[0] == ',' ? 1 : 0) + "}";
}
static std::string jsonErr(const std::string& msg) {
    return "{\"status\":\"error\",\"message\":" + jstr(msg) + "}";
}

static std::string httpRespond(int fd, const std::string& body, int code = 200) {
    const char* status = code == 200 ? "200 OK" : code == 404 ? "404 Not Found" : "400 Bad Request";
    std::ostringstream h;
    h << "HTTP/1.1 " << status << "\r\n"
      << "Content-Type: application/json\r\n"
      << "Access-Control-Allow-Origin: *\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Connection: close\r\n\r\n"
      << body;
    std::string resp = h.str();
    ssize_t sent = 0;
    while (sent < (ssize_t)resp.size()) {
        ssize_t n = send(fd, resp.data() + sent, resp.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) break;
        sent += n;
    }
    ::close(fd);
    return body;
}

RpcServer::RpcServer(Blockchain& bc, Mempool& mp, P2PNode& p2p) : bc_(bc), mp_(mp), p2p_(p2p) {}
RpcServer::~RpcServer() { stop(); }

bool RpcServer::start(uint16_t port) {
    port_ = port;
    listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) return false;
    int opt = 1;
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // hanya localhost — aman
    addr.sin_port = htons(port);
    if (bind(listenFd_, (sockaddr*)&addr, sizeof(addr)) < 0 || listen(listenFd_, 16) < 0) {
        ::close(listenFd_); listenFd_ = -1;
        return false;
    }
    running_ = true;
    std::thread([this] { acceptLoop(); }).detach();
    std::cout << "[RPC] http://127.0.0.1:" << port << "\n";
    return true;
}

void RpcServer::stop() {
    running_ = false;
    if (listenFd_ >= 0) { ::close(listenFd_); listenFd_ = -1; }
}

void RpcServer::acceptLoop() {
    while (running_) {
        int fd = accept(listenFd_, nullptr, nullptr);
        if (fd < 0) { if (running_) continue; else break; }
        std::thread([this, fd] { handleClient(fd); }).detach();
    }
}

void RpcServer::handleClient(int fd) {
    char buf[65536];
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) { ::close(fd); return; }
    buf[n] = 0;
    std::string req(buf);

    std::istringstream iss(req);
    std::string method, path, queryString;
    iss >> method >> path;
    size_t qm = path.find('?');
    if (qm != std::string::npos) { queryString = path.substr(qm + 1); path = path.substr(0, qm); }

    // body = setelah \r\n\r\n
    std::string body;
    size_t bp = req.find("\r\n\r\n");
    if (bp != std::string::npos) body = req.substr(bp + 4);

    std::lock_guard<std::mutex> lk(bcMtx_);
    std::string resp = route(method, path, body, queryString);
    int code = resp.find("\"error\"") != std::string::npos ? 400 : 200;
    if (resp == jsonErr("not found")) code = 404;
    httpRespond(fd, resp, code);
}

std::string RpcServer::route(const std::string& method, const std::string& path,
                             const std::string& body, const std::string& query) {
    // ------------------------------------------------------------ GET /api/info
    if (path == "/" || path == "/api/info") return jsonInfo();

    // ------------------------------------------------------------ GET /api/block/<h>
    if (path.rfind("/api/block/", 0) == 0) {
        try { return jsonBlock(std::stoull(path.substr(11))); }
        catch (...) { return jsonErr("invalid height"); }
    }

    // ------------------------------------------------------------ GET /api/tx/<id>
    if (path.rfind("/api/tx/", 0) == 0) return jsonTx(path.substr(8));

    // ------------------------------------------------------------ GET /api/address/<a>
    if (path.rfind("/api/address/", 0) == 0) return jsonAddress(path.substr(13));

    // ------------------------------------------------------------ GET /api/mempool
    if (path == "/api/mempool") return jsonMempool();

    // ------------------------------------------------------------ GET /api/peers
    if (path == "/api/peers") return jsonPeers();

    // ------------------------------------------------------------ POST /api/sendrawtx
    if (path == "/api/sendrawtx" && method == "POST") return jsonSendRaw(body);

    // ------------------------------------------------------------ POST /api/send?from=X&to=Y&amount=Z&data=pesan
    if (path == "/api/send" && method == "POST") return jsonSend(body, query);

    // ------------------------------------------------------------ GET /api/messages?addr=X[&limit=N]
    if (path == "/api/messages") return jsonMessages(query);

    if (path == "/api/pubkey") return jsonPubkey(query);

    // ------------------------------------------------------------ GET /api/mine?addr=X&n=1
    if (path == "/api/mine") return jsonMine(query);

    // ------------------------------------------------------------ pool (stratum-style)
    if (path == "/api/pool/work") return jsonPoolWork();
    if (path == "/api/pool/stats") return jsonPoolStats();
    if (path == "/api/pool/submit" && method == "POST") return jsonPoolSubmit(body);
    if (path == "/api/pool/worker" && method == "POST") return jsonPoolWorker(body);

    return jsonErr("not found");
}

std::string RpcServer::jsonInfo() {
    std::ostringstream o;
    o << "{\"name\":" << jstr(COIN_NAME)
      << ",\"ticker\":" << jstr(TICKER)
      << ",\"height\":" << bc_.height()
      << ",\"difficulty\":" << bc_.difficulty()
      << ",\"halving\":" << bc_.halvingCount(bc_.height() ? bc_.height() - 1 : 0)
      << ",\"total_mined\":\"" << formatCoin(bc_.totalMined()) << "\""
      << ",\"remaining\":\"" << formatCoin(bc_.remaining()) << "\""
      << ",\"reward\":\"" << formatCoin(bc_.rewardForHeight(bc_.height())) << "\""
      << ",\"mempool_size\":" << mp_.size()
      << ",\"peers\":" << p2p_.peers().size()
      << ",\"tip_hash\":" << jstr(bc_.height() ? bc_.tip().hash : "")
      << "}";
    return o.str();
}

std::string RpcServer::jsonBlock(uint64_t height) {
    if (height >= bc_.height()) return jsonErr("block not found");
    const Block& b = bc_.chain()[height];
    std::ostringstream o;
    o << "{\"height\":" << b.index
      << ",\"hash\":" << jstr(b.hash)
      << ",\"prev_hash\":" << jstr(b.prevHash)
      << ",\"merkle_root\":" << jstr(b.merkleRoot)
      << ",\"nonce\":" << b.nonce
      << ",\"difficulty\":" << b.difficulty
      << ",\"timestamp\":" << b.timestamp
      << ",\"tx_count\":" << b.transactions.size()
      << ",\"txs\":[";
    for (size_t i = 0; i < b.transactions.size(); i++) {
        if (i) o << ",";
        o << jstr(b.transactions[i].id);
    }
    o << "]}";
    return o.str();
}

std::string RpcServer::jsonTx(const std::string& txid) {
    // cari di rantai
    for (const auto& b : bc_.chain()) {
        for (const auto& t : b.transactions) {
            if (t.id == txid) {
                std::ostringstream o;
                o << "{\"txid\":" << jstr(t.id)
                  << ",\"block\":" << b.index
                  << ",\"coinbase\":" << (t.isCoinbase() ? "true" : "false")
                  << ",\"vin\":" << t.inputs.size()
                  << ",\"vout\":[";
                for (size_t i = 0; i < t.outputs.size(); i++) {
                    if (i) o << ",";
                    o << "{\"n\":" << i
                      << ",\"address\":" << jstr(t.outputs[i].address)
                      << ",\"value\":\"" << formatCoin(t.outputs[i].amount) << "\"}";
                }
                o << "]";
                if (!t.data.empty()) o << ",\"data\":" << jstr(t.data);
                o << "}";
                return o.str();
            }
        }
    }
    // cari di mempool
    for (const auto& t : mp_.all()) {
        if (t.id == txid) {
            return "{\"txid\":" + jstr(t.id) + ",\"block\":null,\"status\":\"mempool\"}";
        }
    }
    return jsonErr("tx not found");
}

std::string RpcServer::jsonAddress(const std::string& addr) {
    std::ostringstream o;
    o << "{\"address\":" << jstr(addr)
      << ",\"balance\":\"" << formatCoin(bc_.balanceOf(addr)) << "\""
      << ",\"utxos\":[";
    auto utxos = bc_.utxo().allForAddress(addr);
    for (size_t i = 0; i < utxos.size(); i++) {
        if (i) o << ",";
        o << "{\"txid\":" << jstr(utxos[i].first.txid)
          << ",\"vout\":" << utxos[i].first.index
          << ",\"value\":\"" << formatCoin(utxos[i].second.amount) << "\"}";
    }
    o << "]}";
    return o.str();
}

std::string RpcServer::jsonMempool() {
    std::ostringstream o;
    o << "{\"size\":" << mp_.size() << ",\"txs\":[";
    auto txs = mp_.all();
    for (size_t i = 0; i < txs.size(); i++) {
        if (i) o << ",";
        o << "{\"txid\":" << jstr(txs[i].id)
          << ",\"value\":\"" << formatCoin(txs[i].totalOutput()) << "\"}";
    }
    o << "]}";
    return o.str();
}

std::string RpcServer::jsonPeers() {
    std::ostringstream o;
    o << "{\"count\":" << p2p_.peers().size() << ",\"peers\":[";
    auto ps = p2p_.peers();
    for (size_t i = 0; i < ps.size(); i++) {
        if (i) o << ",";
        o << jstr(ps[i]);
    }
    o << "]}";
    return o.str();
}

std::string RpcServer::jsonSendRaw(const std::string& body) {
    // trim whitespace
    size_t a = body.find_first_not_of(" \t\r\n");
    size_t b = body.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return jsonErr("empty body");
    std::string ser = body.substr(a, b - a + 1);

    try {
        Transaction t = Transaction::deserialize(ser);
        if (!bc_.validateTx(t)) return jsonErr("invalid tx");
        mp_.add(t);
        p2p_.broadcastTx(t);
        return "{\"status\":\"ok\",\"txid\":" + jstr(t.id) + "}";
    } catch (const std::exception& e) {
        return jsonErr(std::string("bad tx serialization: ") + e.what());
    }
}

// ---------------------------------------------------------------------------
// POST /api/send?from=X&to=Y&amount=Z  (+ memo opsional)
//   memo sumber: query "data=..." (URL-encoded) ATAU raw body
//   Membangun & menandatangani transaksi pakai wallet lokal node (~/.zyvor/wallet.dat),
//   lalu memasukkan ke mempool + broadcast. Memo kosong = transfer biasa.
static uint64_t rpcParseAmount(const std::string& s) {
    size_t dot = s.find('.');
    std::string whole = (dot == std::string::npos) ? s : s.substr(0, dot);
    std::string frac = (dot == std::string::npos) ? "" : s.substr(dot + 1);
    while (frac.size() < 8) frac += '0';
    frac = frac.substr(0, 8);
    uint64_t w = whole.empty() ? 0 : std::stoull(whole);
    uint64_t fr = frac.empty() ? 0 : std::stoull(frac);
    return w * SATOSHIS_PER_COIN + fr;
}

static std::string urlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            out += (char)std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else if (s[i] == '+') out += ' ';
        else out += s[i];
    }
    return out;
}

// ---------------------------------------------------------------------------
// Indeks addr -> pubkey dari semua tx on-chain (input memuat pubkey pengirim).
// Dipakai untuk enkripsi pesan (ECIES butuh pubkey penerima).
static std::map<std::string, std::string> buildPubkeyIndex(const Blockchain& bc) {
    std::map<std::string, std::string> idx;
    for (const auto& b : bc.chain()) {
        for (const auto& t : b.transactions) {
            if (t.inputs.empty() || t.inputs[0].pubkey.empty()) continue;
            std::string from = publicKeyToAddress(t.inputs[0].pubkey);
            if (!idx.count(from)) idx[from] = t.inputs[0].pubkey;
        }
    }
    return idx;
}

std::string RpcServer::jsonSend(const std::string& body, const std::string& query) {
    std::string from, to, amountStr, memo;
    bool enc = false;
    std::istringstream qs(query);
    std::string kv;
    while (std::getline(qs, kv, '&')) {
        size_t eq = kv.find('=');
        if (eq == std::string::npos) continue;
        std::string k = kv.substr(0, eq), v = urlDecode(kv.substr(eq + 1));
        if (k == "from") from = v;
        else if (k == "to") to = v;
        else if (k == "enc") enc = (v == "1" || v == "true");
        else if (k == "amount") amountStr = v;
        else if (k == "data") memo = v;
    }
    // memo fallback: body (plain text, bukan JSON)
    if (memo.empty()) {
        std::string b = body;
        // trim
        size_t a = b.find_first_not_of(" 	\r\n");
        size_t z = b.find_last_not_of(" 	\r\n");
        if (a != std::string::npos) memo = b.substr(a, z - a + 1);
        if (memo.size() >= 2 && memo.front() == '{') memo.clear(); // body JSON tidak dianggap memo
        if (!memo.empty()) memo = urlDecode(memo);
    }
    if (from.empty() || to.empty() || amountStr.empty())
        return jsonErr("butuh from, to, amount (data/memo opsional)");

    // enkripsi memo (ECIES) pakai pubkey penerima dari index on-chain
    if (enc && !memo.empty()) {
        auto idx = buildPubkeyIndex(bc_);
        auto it = idx.find(to);
        if (it == idx.end())
            return jsonErr("tidak bisa enkripsi: pubkey penerima belum dikenal (dia belum pernah kirim tx)");
        std::string ct = encryptMessage(it->second, memo);
        if (ct.empty()) return jsonErr("enkripsi gagal");
        memo = ct;
    }

    uint64_t amount = 0;
    try { amount = rpcParseAmount(amountStr); } catch (...) { return jsonErr("amount tidak valid"); }
    if (amount == 0) return jsonErr("amount tidak valid");
    if (memo.size() > MAX_TX_DATA_BYTES)
        return jsonErr("Data/memo terlalu besar (maks " + std::to_string(MAX_TX_DATA_BYTES) + " byte)");

    // wallet lokal node
    std::string walletFile;
    {
        const char* home = std::getenv("HOME");
        walletFile = (home ? std::string(home) : ".") + std::string("/.zyvor/wallet.dat");
    }
    std::vector<KeyPair> w;
    {
        std::ifstream f(walletFile);
        if (!f.good()) return jsonErr("wallet tidak ditemukan di node ini (" + walletFile + ")");
        std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::string pass;
        if (const char* p = std::getenv("ZYVOR_WALLET_PASS")) pass = p;
        if (data.rfind("ZYVW1|", 0) == 0) {
            if (pass.empty()) return jsonErr("wallet terenkripsi — set ZYVOR_WALLET_PASS di node");
            if (!decryptWalletFile(walletFile, data, pass))
                return jsonErr("gagal dekripsi wallet (passphrase salah?)");
        }
        std::istringstream iss(data);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.empty()) continue;
            try { w.push_back(keyPairFromPrivate(line)); } catch (...) {}
        }
    }
    const KeyPair* kp = nullptr;
    for (const auto& k : w) if (k.address == from) { kp = &k; break; }
    if (!kp) return jsonErr("alamat pengirim tidak ada di wallet node ini");

    // fee (memo progresif, sama seperti CLI)
    uint64_t fee = MIN_TX_FEE;
    if (memo.size() > DATA_FREE_LIMIT) {
        size_t extra = memo.size() - DATA_FREE_LIMIT;
        fee += ((extra + 999) / 1000) * DATA_FEE_PER_KB;
    }
    uint64_t need = amount + fee;
    auto utxos = bc_.utxo().allForAddress(from);
    uint64_t total = 0;
    std::vector<OutPoint> selected;
    for (const auto& kv2 : utxos) {
        selected.push_back(kv2.first);
        total += kv2.second.amount;
        if (total >= need) break;
    }
    if (total < need)
        return jsonErr("saldo kurang: butuh " + formatCoin(need) + ", tersedia " + formatCoin(total));

    Transaction tx;
    for (const auto& op : selected) {
        TxIn in;
        in.prevOut = op;
        in.pubkey = kp->publicKey;
        tx.inputs.push_back(in);
    }
    tx.outputs.push_back({amount, to});
    uint64_t change = total - amount - fee;
    if (change > 0) tx.outputs.push_back({change, from});
    tx.timestamp = uint64_t(time(nullptr));
    if (!memo.empty()) tx.data = memo;
    tx.id = computeTxId(tx);
    for (auto& in : tx.inputs) in.signature = signMessage(kp->privateKey, tx.id);

    if (!bc_.validateTx(tx)) return jsonErr("transaksi gagal validasi");
    mp_.add(tx);
    p2p_.broadcastTx(tx);

    std::ostringstream o;
    o << "{\"status\":\"ok\",\"txid\":" << jstr(tx.id)
      << ",\"from\":" << jstr(from) << ",\"to\":" << jstr(to)
      << ",\"amount\":\"" << formatCoin(amount) << "\""
      << ",\"fee\":\"" << formatCoin(fee) << "\""
      << ",\"data\":" << jstr(tx.data) << "}";
    return o.str();
}

// ---------------------------------------------------------------------------
// GET /api/messages?addr=X[&limit=N][&dir=in|out|both]
// Pesan (tx ber-data) yang ditujukan ke / dikirim dari addr.
//   in  : hanya pesan masuk (output tx -> addr)
//   out : hanya pesan keluar (input pubkey -> addr)
//   both: keduanya (default)
// Pesan mempool ikut ditampilkan dengan status "pending".
std::string RpcServer::jsonMessages(const std::string& query) {
    std::string addr, dir = "both";
    int limit = 50;
    std::istringstream qs(query);
    std::string kv;
    while (std::getline(qs, kv, '&')) {
        size_t eq = kv.find('=');
        if (eq == std::string::npos) continue;
        std::string k = kv.substr(0, eq), v = urlDecode(kv.substr(eq + 1));
        if (k == "addr") addr = v;
        else if (k == "limit") limit = std::stoi(v);
        else if (k == "dir") dir = v;
    }
    if (addr.empty()) return jsonErr("butuh addr");
    if (limit < 1) limit = 1;
    if (limit > 500) limit = 500;

    struct Msg { std::string from, to, data, txid, status; uint64_t ts; int block; };
    std::vector<Msg> msgs;

    auto consider = [&](const Transaction& t, int block, const std::string& status) {
        if (t.data.empty() || t.isCoinbase()) return;
        // pengirim = alamat dari pubkey input pertama
        std::string from = t.inputs.empty() ? "" : publicKeyToAddress(t.inputs[0].pubkey);
        bool isIn = false, isOut = (from == addr);
        for (const auto& o : t.outputs) if (o.address == addr) { isIn = true; break; }
        bool take = (dir == "in" && isIn) || (dir == "out" && isOut) ||
                    (dir == "both" && (isIn || isOut));
        if (!take) return;
        // penerima utama = output pertama yang bukan kembalian ke pengirim
        std::string to = from;
        for (const auto& o : t.outputs) {
            if (o.address != from) { to = o.address; break; }
        }
        msgs.push_back({from, to, t.data, t.id, status, t.timestamp, block});
    };

    for (const auto& b : bc_.chain())
        for (const auto& t : b.transactions)
            consider(t, (int)b.index, "confirmed");
    for (const auto& t : mp_.all())
        consider(t, -1, "pending");

    // terbaru dulu (blok tertinggi dulu; on-chain sudah urut, tinggal urutkan global by ts desc)
    std::sort(msgs.begin(), msgs.end(), [](const Msg& a, const Msg& b) {
        return a.ts != b.ts ? a.ts > b.ts : a.block > b.block;
    });
    if ((int)msgs.size() > limit) msgs.resize(limit);

    std::ostringstream o;
    o << "{\"addr\":" << jstr(addr) << ",\"count\":" << msgs.size() << ",\"messages\":[";
    for (size_t i = 0; i < msgs.size(); i++) {
        if (i) o << ",";
        o << "{\"from\":" << jstr(msgs[i].from)
          << ",\"to\":" << jstr(msgs[i].to)
          << ",\"data\":" << jstr(msgs[i].data)
          << ",\"txid\":" << jstr(msgs[i].txid)
          << ",\"status\":" << jstr(msgs[i].status)
          << ",\"time\":" << msgs[i].ts
          << ",\"block\":" << msgs[i].block << "}";
    }
    o << "]}";
    return o.str();
}

std::string RpcServer::jsonPubkey(const std::string& query) {
    std::string addr;
    std::istringstream qs(query);
    std::string kv;
    while (std::getline(qs, kv, '&')) {
        size_t eq = kv.find('=');
        if (eq == std::string::npos) continue;
        if (kv.substr(0, eq) == "addr") addr = urlDecode(kv.substr(eq + 1));
    }
    if (addr.empty()) return jsonErr("butuh addr");
    auto idx = buildPubkeyIndex(bc_);
    auto it = idx.find(addr);
    if (it == idx.end())
        return jsonErr("pubkey alamat belum dikenal (alamat belum pernah mengirim tx)");
    return "{\"status\":\"ok\",\"addr\":" + jstr(addr) + ",\"pubkey\":" + jstr(it->second) + "}";
}

std::string RpcServer::jsonMine(const std::string& query) {
    // query format: addr=<alamat>&n=<jumlah>
    std::string addr;
    int n = 1;
    std::istringstream qs(query);
    std::string kv;
    while (std::getline(qs, kv, '&')) {
        size_t eq = kv.find('=');
        if (eq == std::string::npos) continue;
        std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
        if (k == "addr") addr = v;
        else if (k == "n") n = std::stoi(v);
    }
    if (n < 1 || n > 100) return jsonErr("n out of range");

    std::ostringstream o;
    o << "{\"mined\":" << n << ",\"blocks\":[";
    for (int i = 0; i < n; i++) {
        const Block& b = bc_.mineBlock(mp_.all(), addr);
        // buang tx yang sudah masuk blok dari mempool
        std::vector<std::string> ids;
        for (size_t t = 1; t < b.transactions.size(); t++) ids.push_back(b.transactions[t].id);
        mp_.remove(ids);
        p2p_.broadcastBlock(b);
        if (i) o << ",";
        o << "{\"height\":" << b.index << ",\"hash\":" << jstr(b.hash)
          << ",\"txs\":" << b.transactions.size() << "}";
    }
    o << "],\"height\":" << bc_.height() << "}";
    return o.str();
}


// ============================================================================
// Pool (stratum-style over HTTP, PPLNS payout)
// ============================================================================

// JSON field extractor sederhana: "key":"value" atau "key":123
static std::string jsonExtract(const std::string& json, const std::string& key) {
    std::string pat = std::string("\"") + key + "\":";
    size_t p = json.find(pat);
    if (p == std::string::npos) return "";
    p += pat.size();
    while (p < json.size() && json[p] == ' ') p++;
    if (p >= json.size()) return "";
    if (json[p] == '"') {
        size_t e = json.find('"', p + 1);
        return json.substr(p + 1, e - p - 1);
    }
    size_t e = p;
    while (e < json.size() && (isdigit((unsigned char)json[e]) || json[e] == '.' || json[e] == '-')) e++;
    return json.substr(p, e - p);
}

// Share difficulty pool: network difficulty - 4 bit (min. 1 bit).
static uint32_t poolShareDifficulty(uint32_t net) {
    return net > 4 ? net - 4 : 1;
}

std::string RpcServer::jsonPoolWork() {
    std::lock_guard<std::mutex> lk(poolMtx_);
    uint64_t h = bc_.height();
    uint64_t reward = bc_.rewardForHeight(h ? h : 0) + 0; // estimasi tanpa fee
    uint64_t ts = (uint64_t)time(nullptr);
    std::ostringstream o;
    o << "{\"prevHash\":" << jstr(h ? bc_.tip().hash : "")
      << ",\"height\":" << h
      << ",\"difficulty\":" << bc_.difficulty()
      << ",\"shareDifficulty\":" << poolShareDifficulty(bc_.difficulty())
      << ",\"poolAddress\":" << jstr(GENESIS_ADDRESS)
      << ",\"reward\":" << reward
      << ",\"ts\":" << ts
      << "}";
    return o.str();
}

std::string RpcServer::jsonPoolSubmit(const std::string& body) {
    std::string worker = jsonExtract(body, "worker");
    std::string hash = jsonExtract(body, "hash");
    uint64_t nonce = 0, height = 0;
    bool full = body.find("\"full\":true") != std::string::npos;
    try { nonce = std::stoull(jsonExtract(body, "nonce")); } catch (...) {}
    try { height = std::stoull(jsonExtract(body, "height")); } catch (...) {}
    if (worker.empty() || hash.size() != 64) return jsonErr("bad share");

    std::lock_guard<std::mutex> lk(poolMtx_);
    // verifikasi share: hash harus benar2 hasil preimage yang bisa direproduksi
    // (worker: prevHash|worker|nonce) — prevHash saat height di-submit.
    std::string prev = (height >= 1 && height <= bc_.chain().size()) ? bc_.chain()[height-1].hash : "";
    std::string expect = sha256(sha256(prev + "|" + worker + "|" + std::to_string(nonce)));
    if (expect != hash) return jsonErr("share tidak valid (hash mismatch)");

    uint32_t sd = poolShareDifficulty(bc_.difficulty());
    if (!meetsDifficulty(hash, sd)) return jsonErr("share di bawah target pool");

    // dedup
    for (auto& sh : shares_)
        if (sh.worker == worker && sh.nonce == nonce && sh.hash == hash)
            return jsonErr("share duplikat");

    shares_.push_back({worker, hash, nonce, (uint64_t)time(nullptr), full});
    // simpan persist
    std::ofstream pf(poolFile_, std::ios::app);
    pf << worker << "|" << hash << "|" << nonce << "|" << time(nullptr) << "|" << (full?1:0) << "\n";

    if (full) {
        // blok full-difficulty: pool mine dengan coinbase PPLNS nanti; untuk sekarang
        // catat sebagai blok ditemukan — payout dilakukan manual oleh operator pool
        // (wallet pool) via PPLNS setelah blok dikonfirmasi.
        return "{\"status\":\"BLOCK_FOUND\",\"msg\":\"blok full-difficulty dicatat, payout PPLNS berjalan\"}";
    }
    return "{\"status\":\"ok\",\"shares\":" + std::to_string(shares_.size()) + "}";
}

std::string RpcServer::jsonPoolWorker(const std::string& body) {
    std::string worker = jsonExtract(body, "worker");
    std::string address = jsonExtract(body, "address");
    if (worker.empty() || address.empty()) return jsonErr("worker/address kosong");
    std::lock_guard<std::mutex> lk(poolMtx_);
    workerAddr_[worker] = address;
    return "{\"status\":\"ok\",\"worker\":" + jstr(worker) + ",\"address\":" + jstr(address) + "}";
}

std::string RpcServer::jsonPoolStats() {
    std::lock_guard<std::mutex> lk(poolMtx_);
    std::map<std::string, uint64_t> per;
    for (auto& sh : shares_) per[sh.worker]++;
    std::ostringstream o;
    o << "{\"total_shares\":" << shares_.size() << ",\"workers\":{";
    bool first = true;
    for (auto& kv : per) {
        if (!first) o << ",";
        first = false;
        o << jstr(kv.first) << ":" << kv.second;
    }
    o << "},\"share_difficulty\":" << poolShareDifficulty(bc_.difficulty()) << "}";
    return o.str();
}

} // namespace zyv
