#pragma once
#include <atomic>
#include <map>
#include <mutex>
#include <vector>
#include <string>
#include "blockchain.h"
#include "mempool.h"
#include "p2p.h"

namespace zyv {

// ============================================================================
// RPC API + explorer sederhana — HTTP server POSIX socket + pthread.
// Semua endpoint mengembalikan JSON.
//
//   GET /                     -> info koin (explorer home)
//   GET /api/info             -> info node & rantai
//   GET /api/block/<height>   -> detail blok
//   GET /api/tx/<txid>        -> detail transaksi
//   GET /api/address/<addr>   -> saldo + UTXO alamat
//   GET /api/mempool          -> isi mempool
//   GET /api/peers            -> daftar peer P2P
//   POST /api/sendrawtx       -> body: serialisasi transaksi, kirim ke jaringan
// ============================================================================
class RpcServer {
public:
    RpcServer(Blockchain& bc, Mempool& mp, P2PNode& p2p);
    ~RpcServer();

    bool start(uint16_t port);
    void stop();
    bool running() const { return running_.load(); }
    bool autoMine(const std::string& minerAddr);

private:
    void acceptLoop();
    void handleClient(int fd);
    std::string route(const std::string& method, const std::string& path,
                      const std::string& body, const std::string& query = "");

    std::string jsonInfo();
    std::string jsonBlock(uint64_t height);
    std::string jsonTx(const std::string& txid);
    std::string jsonAddress(const std::string& addr);
    std::string jsonMempool();
    std::string jsonPeers();
    std::string jsonSendRaw(const std::string& body);
    std::string jsonSend(const std::string& body, const std::string& query);
    std::string jsonMessages(const std::string& query);
    std::string jsonPubkey(const std::string& query);
    std::string jsonMine(const std::string& query);

    // ---- pool (stratum-style, PPLNS) ----
    struct PoolShare { std::string worker; std::string hash; uint64_t nonce; uint64_t ts; bool full; };
    std::string jsonPoolWork();
    std::string jsonPoolSubmit(const std::string& body);
    std::string jsonPoolStats();
    std::string jsonPoolWorker(const std::string& body);
    std::vector<PoolShare> shares_;
    std::map<std::string,std::string> workerAddr_; // worker -> payout address
    std::mutex poolMtx_;
    uint64_t lastPaidHeight_ = 0;
    std::string poolFile_ = "zyvor_pool.dat";

    Blockchain& bc_;
    Mempool& mp_;
    P2PNode& p2p_;
    uint16_t port_ = 0;
    int listenFd_ = -1;
    std::atomic<bool> running_{false};
    std::mutex bcMtx_; // kunci antar request HTTP (P2P thread juga menyentuh bc_)
};

} // namespace zyv
