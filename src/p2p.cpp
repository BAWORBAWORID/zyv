#include "p2p.h"
#include "block.h"
#include "params.h"

#include <arpa/inet.h>
#include <cstring>
#include <iostream>
#include <netdb.h>
#include <netinet/in.h>
#include <sstream>
#include <sys/socket.h>
#include <thread>
#include <chrono>
#include <unistd.h>

namespace zyv {


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

    running_ = true;
    std::thread([this] { acceptLoop(); }).detach();
    std::thread([this] { syncMonitorLoop(); }).detach();
    if (port == 0)
        std::cout << "[P2P] listening on port " << listenPort_ << " (acak, client-mode)\n";
    else
        std::cout << "[P2P] listening on port " << listenPort_ << "\n";
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
            peers_.push_back({fd, addr});
        }
        std::thread([this, fd, addr] { peerLoop(fd, addr); }).detach();
    }
}

bool P2PNode::connectTo(const std::string& ipPort) {
    {
        std::lock_guard<std::mutex> lk(peersMtx_);
        for (auto& p : peers_) if (p.addr == ipPort) return true; // sudah terhubung
    }

    size_t colon = ipPort.rfind(':');
    if (colon == std::string::npos) return false;
    std::string ip = ipPort.substr(0, colon);
    uint16_t port = uint16_t(std::stoi(ipPort.substr(colon + 1)));

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

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

    {
        std::lock_guard<std::mutex> lk(peersMtx_);
        peers_.push_back({fd, ipPort});
    }
    std::cout << "[P2P] terhubung ke " << ipPort << "\n";

    // handshake
    sendLine(fd, "VERSION|" + std::to_string(bc_.height()) + "|" + std::to_string(listenPort_));

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
// Peer yang height-nya TIDAK SAMA dengan kita 2 putaran berturut-turut → dianggap
// tidak sinkron → koneksi diputus. Peer yang lebih maju tetap dipertahankan
// (kita yang akan sync dari dia via GETBLOCKS).
void P2PNode::syncMonitorLoop() {
    int round = 0;
    while (running_) {
        for (int i = 0; i < 15 && running_; ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!running_) break;
        ++round;
        uint64_t myH = bc_.height();
        std::vector<int> drop;
        {
            std::lock_guard<std::mutex> lk(peersMtx_);
            for (auto& p : peers_) {
                // tandai: kirim SYNC (balasan SYNCACK akan memperbarui lastHeight)
                sendLine(p.fd, "SYNC|" + std::to_string(myH));
                if (round > 1 && p.lastHeight != 0 && p.lastHeight != myH && p.lastHeight < myH)
                    drop.push_back(p.fd);
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
                std::cout << "[P2P] peer " << addr << " dihapus (tidak sinkron, height tertinggal)\n";
            }
        }
    }
}

void P2PNode::handleMessage(int fd, const std::string& line, const std::string& fromAddr) {
    std::istringstream iss(line);
    std::string type;
    std::getline(iss, type, '|');

    if (type == "VERSION") {
        std::string h, port;
        std::getline(iss, h, '|');
        std::getline(iss, port);
        std::cout << "[P2P] peer " << fromAddr << " height=" << h << "\n";
        sendLine(fd, "VERACK|" + std::to_string(bc_.height()));
        // kalau peer lebih maju, minta blok
        if (std::stoull(h) > bc_.height()) sendLine(fd, "GETBLOCKS|" + std::to_string(bc_.height()));
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
            for (auto& p : peers_) if (p.fd == fd) p.lastHeight = std::stoull(h);
        } catch (...) {}
    }
    else if (type == "VERACK") {
        std::string h;
        std::getline(iss, h);
        if (std::stoull(h) > bc_.height()) sendLine(fd, "GETBLOCKS|" + std::to_string(bc_.height()));
    }
    else if (type == "GETBLOCKS") {
        std::string startS;
        std::getline(iss, startS);
        size_t start = std::stoul(startS);
        // kirim maksimal 50 blok per pesan
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
        for (const auto& p : peers_) {
            if (p.addr == fromAddr) continue;
            if (!first) out << ",";
            out << p.addr;
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
            if (!peer.empty()) connectTo(peer);
        }
    }
}

void P2PNode::sendLine(int fd, const std::string& line) {
    std::string msg = line + "\n";
    ssize_t sent = 0;
    while (sent < (ssize_t)msg.size()) {
        ssize_t n = send(fd, msg.data() + sent, msg.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return;
        sent += n;
    }
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
    for (const auto& p : peers_) {
        if (!first) out << ",";
        out << p.addr;
        first = false;
    }
    std::string line = out.str();
    std::lock_guard<std::mutex> lk(peersMtx_);
    for (auto& p : peers_) if (p.fd >= 0) sendLine(p.fd, line);
}

std::vector<std::string> P2PNode::peers() const {
    std::lock_guard<std::mutex> lk(peersMtx_);
    std::vector<std::string> res;
    for (auto& p : peers_) res.push_back(p.addr);
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
