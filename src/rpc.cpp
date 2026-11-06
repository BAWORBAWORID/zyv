#include "rpc.h"
#include "block.h"
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

    // ------------------------------------------------------------ GET /api/mine?addr=X&n=1
    if (path == "/api/mine") return jsonMine(query);

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
                o << "]}";
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

} // namespace zyv
