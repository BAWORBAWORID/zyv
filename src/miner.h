#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace zyv {

// ============================================================================
// Miner daemon — solo mining ala Bitcoin (poll mempool + auto-mine).
//
// Mode:
//   1. SOLO  : mine loop lokal (blockchain & mempool in-process).
//   2. POOL  : worker stratum — hash ke arah share-target pool, submit share
//              via HTTP ke pool server; saat menemukan blok full-difficulty,
//              blok dikirim ke pool (mine share > network difficulty).
//
// Stratum sederhana (HTTP JSON, bukan TCP JSON-RPC):
//   GET  /api/pool/work           -> job: prevHash, height, merkleTemplate,
//                                    difficulty, shareDifficulty, poolAddress
//   POST /api/pool/submit         -> body: JSON share
//                                    {worker, nonce, hash, height, full}
//                                    full=true berarti blok lengkap (wire format)
//   GET  /api/pool/stats          -> share per worker (PPLNS window)
//   POST /api/pool/worker         -> {worker, address} registrasi alamat payout
// ============================================================================

struct PoolJob {
    std::string prevHash;
    uint64_t height = 0;
    uint32_t difficulty = 0;      // network difficulty
    uint32_t shareDifficulty = 0; // pool share difficulty (lebih rendah)
    std::string poolAddress;      // alamat payout pool (coinbase saat pool mine)
    uint64_t reward = 0;          // reward + fees diperkirakan
};

class MinerDaemon {
public:
    MinerDaemon() = default;

    // SOLO: poll mempool setiap pollIntervalSec detik; bila ada tx (atau
    // alwaysMine=true), mine blok baru dengan reward ke minerAddress.
    // Berhenti saat stop() dipanggil.
    void runSolo(class Blockchain& bc, class Mempool& mp,
                 const std::string& minerAddress, int pollIntervalSec,
                 bool alwaysMine, std::atomic<bool>& stopFlag);

    // POOL worker: poll job dari poolUrl, hash share, submit.
    void runPoolWorker(const std::string& poolUrl, const std::string& workerName,
                       const std::string& payoutAddress,
                       std::atomic<bool>& stopFlag);

    void stop() { stopFlag_ = true; }

private:
    std::atomic<bool> stopFlag_{false};
    // POOL helper: bangun blok kandidat dari job + cari nonce share
    std::string poolSubmit(const std::string& poolUrl, const std::string& body);
    PoolJob poolFetchWork(const std::string& poolUrl);
};

} // namespace zyv
