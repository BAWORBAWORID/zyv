#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include "blockchain.h"
#include "mempool.h"

namespace zyv {

// ============================================================================
// P2P antar node — TCP line-based, POSIX socket + pthread.
//
// Pesan (satu baris JSON sederhana, '|'-separated untuk simplicity):
//   VERSION|<height>|<listenPort>      handshake saat connect
//   VERACK|<height>                    balasan handshake
//   GETBLOCKS|<startHeight>            minta blok dari tinggi X
//   BLOCKS|<count>|<blok1>..<blokN>    kirim blok (serialisasi block.h)
//   TX|<txSerialized>                  broadcast transaksi baru
//   NEWBLOCK|<blokSerialized>          broadcast blok baru
//   GETPEERS                           minta daftar peer
//   PEERS|<ip:port>,<ip:port>..        daftar peer
// ============================================================================
class P2PNode {
public:
    P2PNode(Blockchain& bc, Mempool& mp);
    ~P2PNode();

    // Mulai listen di port `port` (thread terpisah). Return false bila gagal bind.
    bool start(uint16_t port);
    uint16_t port() const { return listenPort_; }

    // Sambungkan ke peer `ip:port` dan lakukan handshake.
    bool connectTo(const std::string& ipPort);

    // Broadcast blok/tx/peer ke semua peer yang dikenal.
    void broadcastBlock(const Block& b);
    void broadcastTx(const Transaction& t);
    void broadcastPeers();

    std::vector<std::string> peers() const;

    void stop();

    // Sync satu sesi (CLI): konek ke host:port, minta blok dari height kita, terima & apply.
    // Return: jumlah blok yang berhasil diterima (-1 bila konek gagal).
    static int syncOneShot(const std::string& hostPort, Blockchain& bc);

private:
    struct Peer {
        int fd = -1;
        std::string addr; // "ip:port"
        uint64_t lastHeight = 0; // height terakhir yang dilaporkan peer (sync monitor)
        int missed = 0;          // berapa putaran SYNC tanpa SYNCACK (respons)
    };

    void acceptLoop();
    void syncMonitorLoop();
    void peerLoop(int fd, const std::string& addr);
    void handleMessage(int fd, const std::string& line, const std::string& fromAddr);
    bool sendLine(int fd, const std::string& line);
    bool sendTo(const std::string& addr, const std::string& line);

    Blockchain& bc_;
    Mempool& mp_;
    uint16_t listenPort_ = 0;
    int listenFd_ = -1;
    std::atomic<bool> running_{false};
    mutable std::mutex peersMtx_;
    std::vector<Peer> peers_;
};

} // namespace zyv
