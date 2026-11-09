#include "p2p.h"
#include "block.h"
#include "params.h"

#include <arpa/inet.h>
#include <cstring>
#include <iostream>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <curl/curl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sstream>
#include <sys/socket.h>
#include <thread>
#include <chrono>
#include <unistd.h>

namespace zyv {

static bool isLoopback(const std::string& addr) {
    std::string host = addr;
    size_t colon = addr.rfind(':');
    if (colon != std::string::npos) host = addr.substr(0, colon);
    if (host == "localhost" || host == "127.0.0.1" || host == "0.0.0.0" || host == "::1") return true;
    if (host.rfind("127.", 0) == 0) return true;
    return false;
}

static bool isValidPeerAddr(const std::string& addr) {
    if (addr.empty() || addr.size() > 255) return false;
    size_t colon = addr.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon == addr.size() - 1) return false;
    std::string portStr = addr.substr(colon + 1);
    for (char c : portStr) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    try {
        int p = std::stoi(portStr);
        if (p <= 0 || p > 65535) return false;
    } catch (...) {
        return false;
    }
    std::string host = addr.substr(0, colon);
    for (char c : host) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-' && c != '_')
            return false;
    }
    return true;
}

static std::string extractIp(const std::string& addr) {
    size_t colon = addr.rfind(':');
    if (colon == std::string::npos) return addr;
    return addr.substr(0, colon);
}

static bool isPublicIp(const std::string& ip) {
    if (isLoopback(ip)) return false;
    if (ip.rfind("10.", 0) == 0) return false;
    if (ip.rfind("192.168.", 0) == 0) return false;
    if (ip.rfind("169.254.", 0) == 0) return false;
    if (ip.rfind("172.", 0) == 0) {
        size_t d1 = ip.find('.');
        size_t d2 = ip.find('.', d1 + 1);
        if (d1 != std::string::npos && d2 != std::string::npos) {
            try {
                int second = std::stoi(ip.substr(d1 + 1, d2 - d1 - 1));
                if (second >= 16 && second <= 31) return false;
            } catch (...) {}
        }
    }
    return true;
}

static size_t p2pCurlWrite(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

static std::string curlHttpGet(const std::string& url, long timeoutSec = 5) {
    CURL* c = curl_easy_init();
    if (!c) return "";
    std::string out;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_USERAGENT, "zyvor-node/1.0");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, p2pCurlWrite);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeoutSec);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_perform(c);
    curl_easy_cleanup(c);
    return out;
}

static std::string parseIpFromJson(const std::string& json) {
    size_t pos = json.find("\"ip\":");
    if (pos == std::string::npos) pos = json.find("\"ip\" :");
    if (pos == std::string::npos) return "";
    size_t q1 = json.find('"', pos + 5);
    if (q1 == std::string::npos) return "";
    size_t q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) return "";
    return json.substr(q1 + 1, q2 - q1 - 1);
}

std::string P2PNode::fetchPublicIp() {
    static std::string cached;
    static std::mutex mtx;
    std::lock_guard<std::mutex> lk(mtx);
    if (!cached.empty()) return cached;

    // 1. Dapatkan dari https://ipinfo.io/json
    std::string res = curlHttpGet("https://ipinfo.io/json", 4);
    std::string ip = parseIpFromJson(res);

    // 2. Fallback jika ipinfo.io gagal: https://api.ipify.org
    if (ip.empty() || isLoopback(ip) || !isPublicIp(ip)) {
        res = curlHttpGet("https://api.ipify.org", 4);
        while (!res.empty() && (res.back() == '\r' || res.back() == '\n' || res.back() == ' '))
            res.pop_back();
        if (!res.empty() && !isLoopback(res) && isPublicIp(res)) ip = res;
    }

    if (!ip.empty() && isPublicIp(ip)) {
        cached = ip;
        return ip;
    }
    return "";
}

std::vector<std::string> P2PNode::resolveDnsSeed(const std::string& host, uint16_t port) {
    std::vector<std::string> results;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) == 0 && res) {
        for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
            sockaddr_in* sin = (sockaddr_in*)p->ai_addr;
            char ipBuf[64];
            if (inet_ntop(AF_INET, &sin->sin_addr, ipBuf, sizeof(ipBuf))) {
                std::string ipStr = ipBuf;
                if (!isLoopback(ipStr)) {
                    std::string ep = ipStr + ":" + std::to_string(port);
                    if (std::find(results.begin(), results.end(), ep) == results.end()) {
                        results.push_back(ep);
                    }
                }
            }
        }
        freeaddrinfo(res);
    }
    return results;
}

std::string P2PNode::publicIp() const {
    return publicIp_;
}

std::string P2PNode::externalAddress() const {
    const char* env = std::getenv("ZYVOR_EXTERNAL_ADDR");
    if (env && *env) return std::string(env);
    if (!publicIp_.empty() && listenPort_ > 0) {
        return publicIp_ + ":" + std::to_string(listenPort_);
    }
    return "";
}


// Serialisasi blok jadi SATU baris (header + tiap tx dipisah spasi-free '|NL|'):
static std::string serializeBlockLine(const Block& b) {
    std::string s = serializeBlock(b);
    // ganti newline dengan unit separator 0x01
    std::string out;
    for (char c : s) out += (c == '\n') ? std::string(1, '\x01') : std::string(1, c);
    if (!out.empty() && out.back() == '\x01') out.pop_back();
    return out;
}
static Block parseBlockLine(const std::string& line) {
    std::string s;
    for (char c : line) s += (c == '\x01') ? std::string(1, '\n') : std::string(1, c);
    std::istringstream bs(s);
    Block b;
    if (!readBlock(bs, b)) throw std::runtime_error("bad block");
    return b;
}

P2PNode::P2PNode(Blockchain& bc, Mempool& mp) : bc_(bc), mp_(mp) {}

P2PNode::~P2PNode() { stop(); }

bool P2PNode::start(uint16_t port) {
    listenPort_ = port;
    listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) return false;

    int opt = 1;
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(listenFd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }
    if (listen(listenFd_, 16) < 0) {
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }

    // Ambil port AKTUAL (port 0 = OS pilih acak)
    sockaddr_in actual{};
    socklen_t alen = sizeof(actual);
    if (getsockname(listenFd_, (sockaddr*)&actual, &alen) == 0)
        listenPort_ = ntohs(actual.sin_port);

    // Dapatkan dan verifikasi IP Publik server (via ipinfo.io / apiify)
    publicIp_ = fetchPublicIp();

    running_ = true;
    std::thread([this] { acceptLoop(); }).detach();
    std::thread([this] { syncMonitorLoop(); }).detach();
    if (port == 0)
        std::cout << "[P2P] listening on port " << listenPort_ << " (acak, client-mode)\n";
    else
        std::cout << "[P2P] listening on port " << listenPort_ << "\n";
    if (!publicIp_.empty())
        std::cout << "[P2P] IP Publik terverifikasi: " << publicIp_ << "\n";
    return true;
}

void P2PNode::stop() {
    running_ = false;
    if (listenFd_ >= 0) { ::close(listenFd_); listenFd_ = -1; }
    std::lock_guard<std::mutex> lk(peersMtx_);
    for (auto& p : peers_) if (p.fd >= 0) ::close(p.fd);
    peers_.clear();
}

void P2PNode::acceptLoop() {
    while (running_) {
        sockaddr_in cli{};
        socklen_t len = sizeof(cli);
        int fd = accept(listenFd_, (sockaddr*)&cli, &len);
        if (fd < 0) { if (running_) continue; else break; }

        char ip[64];
        inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof(ip));
        std::string addr = std::string(ip) + ":" + std::to_string(ntohs(cli.sin_port));
        {
            std::lock_guard<std::mutex> lk(peersMtx_);
            // Koneksi masuk: belum dianggap routable sampai ada VERSION dengan port listening yang valid
            peers_.push_back({fd, addr, "", false, 0, 0});
        }
        std::thread([this, fd, addr] { peerLoop(fd, addr); }).detach();
    }
}

bool P2PNode::connectTo(const std::string& ipPort) {
    if (!isValidPeerAddr(ipPort) || isLoopback(ipPort)) return false;
    {
        std::lock_guard<std::mutex> lk(peersMtx_);
        for (auto& p : peers_) {
            if (p.addr == ipPort || (!p.routableAddr.empty() && p.routableAddr == ipPort)) {
                return true; // sudah terhubung
            }
        }
    }

    size_t colon = ipPort.rfind(':');
    if (colon == std::string::npos) return false;
    std::string ip = ipPort.substr(0, colon);
    uint16_t port = 0;
    try {
        port = uint16_t(std::stoi(ipPort.substr(colon + 1)));
    } catch (...) {
        return false;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    // Timeout saat handshake koneksi
    timeval tv{5, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    hostent* he = gethostbyname(ip.c_str());
    if (!he) { ::close(fd); return false; }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

    if (connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        ::close(fd);
        std::cout << "[P2P] gagal sambung " << ipPort << "\n";
        return false;
    }

    timeval tvZero{0, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tvZero, sizeof(tvZero));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tvZero, sizeof(tvZero));

    {
        std::lock_guard<std::mutex> lk(peersMtx_);
        peers_.push_back({fd, ipPort, ipPort, true, 0, 0});
        knownPeers_.insert(ipPort);
    }
    std::cout << "[P2P] terhubung ke " << ipPort << "\n";

    // Handshake VERSION
    std::string adv = externalAddress();
    std::string vmsg = "VERSION|" + std::to_string(bc_.height()) + "|" + std::to_string(listenPort_);
    if (!adv.empty()) vmsg += "|" + adv;
    sendLine(fd, vmsg);

    // Langsung minta daftar peer
    sendLine(fd, "GETPEERS");

    std::thread([this, fd, ipPort] { peerLoop(fd, ipPort); }).detach();
    return true;
}

void P2PNode::peerLoop(int fd, const std::string& addr) {
    std::string buf;
    char tmp[4096];
    while (running_) {
        ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        buf.append(tmp, size_t(n));
        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            if (!line.empty()) handleMessage(fd, line, addr);
        }
    }
    ::close(fd);
    {
        std::lock_guard<std::mutex> lk(peersMtx_);
        for (auto it = peers_.begin(); it != peers_.end(); ++it) {
            if (it->fd == fd) { peers_.erase(it); break; }
        }
    }
    std::cout << "[P2P] peer " << addr << " putus\n";
}

// Monitor sinkron di latar belakang: tiap 15 detik tanya height tiap peer.
void P2PNode::syncMonitorLoop() {
    int round = 0;
    while (running_) {
        for (int i = 0; i < 15 && running_; ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!running_) break;
        round++;

        uint64_t myH = bc_.height();
        std::vector<int> drop;
        {
            std::lock_guard<std::mutex> lk(peersMtx_);
            for (auto& p : peers_) {
                if (sendLine(p.fd, "SYNC|" + std::to_string(myH))) {
                    ++p.missed; // di-reset ke 0 saat SYNCACK diterima
                    if (p.missed >= 3) drop.push_back(p.fd); // ~45 dtk tanpa respons
                } else {
                    drop.push_back(p.fd); // socket langsung gagal kirim → mati
                }
            }
        }
        for (int fd : drop) {
            std::string addr;
            {
                std::lock_guard<std::mutex> lk(peersMtx_);
                for (auto it = peers_.begin(); it != peers_.end(); ++it)
                    if (it->fd == fd) { addr = it->addr; peers_.erase(it); break; }
            }
            if (!addr.empty()) {
                ::close(fd);
                std::cout << "[P2P] peer " << addr << " dihapus (tidak merespons ~45 detik)\n";
            }
        }

        // Tiap ~60 detik (4 putaran), request daftar peer terbaru dari semua peer yang terhubung
        if (round % 4 == 0) {
            queryPeers();
        }
    }
}

void P2PNode::handleMessage(int fd, const std::string& line, const std::string& fromAddr) {
    std::istringstream iss(line);
    std::string type;
    std::getline(iss, type, '|');

    if (type == "VERSION") {
        std::string h, portStr, advAddr;
        std::getline(iss, h, '|');
        std::getline(iss, portStr, '|');
        std::getline(iss, advAddr);
        uint16_t peerListenPort = 0;
        try { if (!portStr.empty()) peerListenPort = uint16_t(std::stoi(portStr)); } catch (...) {}

        std::cout << "[P2P] peer " << fromAddr << " height=" << h;
        if (peerListenPort > 0) std::cout << " port=" << peerListenPort;
        if (!advAddr.empty()) std::cout << " adv=" << advAddr;
        std::cout << "\n";

        sendLine(fd, "VERACK|" + std::to_string(bc_.height()));

        // Tentukan alamat routable milik peer ini
        std::string routable;
        if (!advAddr.empty() && isValidPeerAddr(advAddr) && !isLoopback(advAddr)) {
            routable = advAddr;
        } else if (peerListenPort > 0) {
            std::string ip = extractIp(fromAddr);
            if (isPublicIp(ip)) {
                routable = ip + ":" + std::to_string(peerListenPort);
            }
        }

        {
            std::lock_guard<std::mutex> lk(peersMtx_);
            for (auto& p : peers_) {
                if (p.fd == fd) {
                    if (!routable.empty()) {
                        p.routableAddr = routable;
                        p.isRoutable = true;
                    } else {
                        p.isRoutable = false;
                    }
                    break;
                }
            }
        }
        if (!routable.empty()) {
            addKnownPeer(routable);
        }

        // Minta daftar peer dari remote
        sendLine(fd, "GETPEERS");

        // kalau peer lebih maju, minta blok
        try {
            if (std::stoull(h) > bc_.height()) sendLine(fd, "GETBLOCKS|" + std::to_string(bc_.height()));
        } catch (...) {}
    }
    else if (type == "SYNC") {
        // monitor sinkron: peer tanya height kita
        std::string h;
        std::getline(iss, h);
        try {
            std::lock_guard<std::mutex> lk(peersMtx_);
            for (auto& p : peers_) if (p.fd == fd) p.lastHeight = std::stoull(h);
        } catch (...) {}
        sendLine(fd, "SYNCACK|" + std::to_string(bc_.height()));
    }
    else if (type == "SYNCACK") {
        std::string h;
        std::getline(iss, h);
        try {
            std::lock_guard<std::mutex> lk(peersMtx_);
            for (auto& p : peers_)
                if (p.fd == fd) { p.lastHeight = std::stoull(h); p.missed = 0; }
        } catch (...) {}
    }
    else if (type == "VERACK") {
        std::string h;
        std::getline(iss, h);
        // Handshake selesai: minta daftar peer
        sendLine(fd, "GETPEERS");
        try {
            if (std::stoull(h) > bc_.height()) sendLine(fd, "GETBLOCKS|" + std::to_string(bc_.height()));
        } catch (...) {}
    }
    else if (type == "GETBLOCKS") {
        std::string startS;
        std::getline(iss, startS);
        size_t start = 0;
        try { start = std::stoul(startS); } catch (...) { return; }
        size_t count = std::min<size_t>(50, bc_.height() > start ? bc_.height() - start : 0);
        for (size_t i = start; i < start + count; i++) {
            sendLine(fd, std::string("BLOCK|") + serializeBlockLine(bc_.chain()[i]));
        }
    }
    else if (type == "BLOCK") {
        std::string ser;
        std::getline(iss, ser);
        Block b;
        try { b = parseBlockLine(ser); } catch (...) { return; }
        if (bc_.addBlock(b)) {
            std::cout << "[SYNC] blok #" << b.index << " diterima (height="
                      << bc_.height() << ")\n";
        }
    }
    else if (type == "TX") {
        std::string ser;
        std::getline(iss, ser);
        try {
            Transaction t = Transaction::deserialize(ser);
            if (bc_.validateTx(t) && mp_.add(t)) {
                broadcastTx(t); // relay
            }
        } catch (...) {}
    }
    else if (type == "NEWBLOCK") {
        std::string ser;
        std::getline(iss, ser);
        Block b;
        try { b = parseBlockLine(ser); } catch (...) { return; }
        if (bc_.addBlock(b)) {
            broadcastBlock(b); // relay
        }
    }
    else if (type == "GETPEERS") {
        std::ostringstream out;
        out << "PEERS|";
        bool first = true;
        std::set<std::string> pool;
        {
            std::lock_guard<std::mutex> lk(peersMtx_);
            for (const auto& p : peers_) {
                if (p.isRoutable && !p.routableAddr.empty() && p.routableAddr != fromAddr && p.addr != fromAddr) {
                    if (isValidPeerAddr(p.routableAddr) && !isLoopback(p.routableAddr))
                        pool.insert(p.routableAddr);
                }
            }
            for (const auto& kp : knownPeers_) {
                if (kp != fromAddr && isValidPeerAddr(kp) && !isLoopback(kp))
                    pool.insert(kp);
            }
        }
        for (const auto& addr : pool) {
            if (!first) out << ",";
            out << addr;
            first = false;
        }
        sendLine(fd, out.str());
    }
    else if (type == "PEERS") {
        std::string list;
        std::getline(iss, list);
        std::stringstream ps(list);
        std::string peer;
        while (std::getline(ps, peer, ',')) {
            while (!peer.empty() && (peer.back() == '\r' || peer.back() == ' ' || peer.back() == '\t'))
                peer.pop_back();
            while (!peer.empty() && (peer.front() == ' ' || peer.front() == '\t'))
                peer.erase(0, 1);
            if (peer.empty()) continue;

            // Validasi alamat peer: bukan loopback / localhost / invalid
            if (!isValidPeerAddr(peer) || isLoopback(peer)) continue;

            addKnownPeer(peer);
            connectTo(peer);
        }
    }
}

void P2PNode::addKnownPeer(const std::string& addr) {
    if (!isValidPeerAddr(addr) || isLoopback(addr)) return;
    std::lock_guard<std::mutex> lk(peersMtx_);
    knownPeers_.insert(addr);
}

std::vector<std::string> P2PNode::knownPeers() const {
    std::lock_guard<std::mutex> lk(peersMtx_);
    return std::vector<std::string>(knownPeers_.begin(), knownPeers_.end());
}

void P2PNode::loadKnownPeers(const std::string& path) {
    std::ifstream f(path);
    if (!f.good()) return;
    std::string line;
    std::lock_guard<std::mutex> lk(peersMtx_);
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
            line.erase(0, 1);
        if (line.empty() || line[0] == '#') continue;
        if (isValidPeerAddr(line) && !isLoopback(line)) {
            knownPeers_.insert(line);
        }
    }
}

void P2PNode::saveKnownPeers(const std::string& path) const {
    std::lock_guard<std::mutex> lk(peersMtx_);
    if (knownPeers_.empty()) return;
    std::ofstream f(path, std::ios::trunc);
    if (!f.good()) return;
    for (const auto& p : knownPeers_) {
        f << p << "\n";
    }
}

void P2PNode::queryPeers() {
    std::lock_guard<std::mutex> lk(peersMtx_);
    for (auto& p : peers_) {
        if (p.fd >= 0) {
            sendLine(p.fd, "GETPEERS");
        }
    }
}

bool P2PNode::sendLine(int fd, const std::string& line) {
    std::string msg = line + "\n";
    ssize_t sent = 0;
    while (sent < (ssize_t)msg.size()) {
        ssize_t n = send(fd, msg.data() + sent, msg.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

bool P2PNode::sendTo(const std::string& addr, const std::string& line) {
    std::lock_guard<std::mutex> lk(peersMtx_);
    for (auto& p : peers_) {
        if (p.addr == addr && p.fd >= 0) { sendLine(p.fd, line); return true; }
    }
    return false;
}

void P2PNode::broadcastBlock(const Block& b) {
    std::string line = "NEWBLOCK|" + serializeBlockLine(b);
    std::lock_guard<std::mutex> lk(peersMtx_);
    for (auto& p : peers_) if (p.fd >= 0) sendLine(p.fd, line);
}

void P2PNode::broadcastTx(const Transaction& t) {
    std::string line = "TX|" + t.serialize();
    std::lock_guard<std::mutex> lk(peersMtx_);
    for (auto& p : peers_) if (p.fd >= 0) sendLine(p.fd, line);
}

void P2PNode::broadcastPeers() {
    std::ostringstream out;
    out << "PEERS|";
    bool first = true;
    std::set<std::string> pool;
    {
        std::lock_guard<std::mutex> lk(peersMtx_);
        for (const auto& p : peers_) {
            if (p.isRoutable && !p.routableAddr.empty()) {
                if (isValidPeerAddr(p.routableAddr) && !isLoopback(p.routableAddr))
                    pool.insert(p.routableAddr);
            }
        }
        for (const auto& kp : knownPeers_) {
            if (isValidPeerAddr(kp) && !isLoopback(kp)) pool.insert(kp);
        }
    }
    for (const auto& p : pool) {
        if (!first) out << ",";
        out << p;
        first = false;
    }
    std::string line = out.str();
    std::lock_guard<std::mutex> lk(peersMtx_);
    for (auto& p : peers_) if (p.fd >= 0) sendLine(p.fd, line);
}

std::vector<std::string> P2PNode::peers() const {
    std::lock_guard<std::mutex> lk(peersMtx_);
    std::vector<std::string> res;
    for (auto& p : peers_) {
        if (!p.routableAddr.empty() && !isLoopback(p.routableAddr)) {
            res.push_back(p.routableAddr);
        } else if (!isLoopback(p.addr)) {
            res.push_back(p.addr);
        }
    }
    return res;
}

// ----------------------------------------------------------------------------
// Sync satu sesi utk CLI (wallet/balance auto-sync): tanpa start node penuh.
int P2PNode::syncOneShot(const std::string& hostPort, Blockchain& bc) {
    size_t c = hostPort.find(':');
    if (c == std::string::npos) return -1;
    std::string host = hostPort.substr(0, c);
    int port = std::stoi(hostPort.substr(c + 1));

    addrinfo hints{}; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res)
        return -1;
    int fd = socket(res->ai_family, res->ai_socktype, 0);
    if (fd < 0 || connect(fd, res->ai_addr, res->ai_addrlen) < 0) { freeaddrinfo(res); return -1; }
    freeaddrinfo(res);

    auto sendLine = [&](const std::string& line) {
        std::string out = line + "\n";
        ssize_t n = send(fd, out.data(), out.size(), MSG_NOSIGNAL);
        (void)n;
    };
    // handshake
    sendLine("VERSION|" + std::to_string(bc.height()) + "|0");
    // baca balasan + blok sampai timeout singkat
    int received = 0;
    std::string buf;
    char tmp[65536];
    // set recv timeout 5 dtk
    timeval tv{5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    bool gotVerack = false;
    bool requested = false;
    int idleRounds = 0;
    while (true) {
        ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        buf.append(tmp, n);
        size_t p;
        while ((p = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, p);
            buf.erase(0, p + 1);
            if (line.rfind("VERACK|", 0) == 0) {
                gotVerack = true;
                size_t peerH = std::stoull(line.substr(7));
                if (peerH > bc.height()) sendLine("GETBLOCKS|" + std::to_string(bc.height()));
                else { close(fd); return received; }
                requested = true;
            } else if (line.rfind("BLOCK|", 0) == 0) {
                try {
                    std::string ser;
                    for (char ch : line.substr(6)) ser += (ch == '\x01') ? '\n' : ch;
                    std::istringstream bs(ser);
                    Block b;
                    if (readBlock(bs, b) && bc.addBlock(b)) received++;
                } catch (...) {}
            }
        }
        if (gotVerack && requested && buf.empty()) {
            // tidak ada blok lagi di buffer — server sudah tidak mengirim apa pun:
            // tandai idle & keluar (server kirim maks 50 blok per ronde; ronde berikutnya
            // via auto-reconnect/retry node, jangan spin GETBLOCKS tanpa akhir)
            if (idleRounds++ >= 1) break;
            sendLine("GETBLOCKS|" + std::to_string(bc.height()));
        }
    }
    close(fd);
    return received;
}

} // namespace zyv
