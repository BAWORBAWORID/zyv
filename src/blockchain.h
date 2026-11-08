#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "block.h"
#include "transaction.h"
#include "params.h"
#include "utxo.h"

namespace zyv {

// Helper: txid = SHA256(SHA256(serializeUnsigned()))
std::string computeTxId(const Transaction& tx);

// ============================================================================
// Blockchain Zyvor Coin — rantai blok + UTXO + mining + halving + validasi.
// ============================================================================
class Blockchain {
public:
    Blockchain();

    void createGenesis();

    // Tambang satu blok berisi transaksi `txs` untuk `minerAddress`.
    // Transaksi yang tidak valid akan dilewati.
    const Block& mineBlock(const std::vector<Transaction>& txs, const std::string& minerAddress);

    // Reward (zyvor) untuk blok pada tinggi tertentu (setelah halving).
    uint64_t rewardForHeight(uint64_t height) const;

    uint64_t totalMined() const { return minedSupply_; }
    uint64_t remaining() const { return MAX_SUPPLY_ZYVOR - minedSupply_; }
    int halvingCount(uint64_t height) const { return int(height / HALVING_INTERVAL); }

    // Validasi seluruh rantai + rebuild UTXO set (mutasi utxo_ hanya jika valid).
    bool validateChain();

    // Checkpoint anti-reorg: hash di tinggi tertentu yang diketahui benar.
    // Blok pada tinggi checkpoint yang hash-nya berbeda = rantai palsu, ditolak.
    static const std::map<uint64_t, std::string>& checkpoints();
    static bool checkpointMismatch(uint64_t index, const std::string& hash);

    // Validasi satu transaksi terhadap UTXO saat ini (signature + no double-spend).
    bool validateTx(const Transaction& tx) const { return validateTx(tx, utxo_); }
    bool validateTx(const Transaction& tx, const UtxoSet& ut) const;

    // Buang tx mempool yang tidak valid lagi terhadap UTXO saat ini.
    // Return jumlah yang dibuang.
    size_t purgeMempool(std::vector<Transaction>& pool) const;

    // Tambahkan blok eksternal (dari jaringan) bila valid & menyambung ke tip.
    // Bila blok menyambung ke blok lama (fork), disimpan di orphan pool;
    // rantai alternatif yang punya TOTAL WORK lebih besar akan menggantikan rantai aktif (reorg).
    bool addBlock(const Block& b);

    // Total work kumulatif rantai aktif (jumlah 2^difficulty per blok, dibatasi uint64).
    uint64_t totalWork() const { return totalWork_; }

    // Jumlah blok yatim menunggu di orphan pool.
    size_t orphanCount() const { return orphans_.size(); }

    // Ganti rantai dengan versi serialized yang lebih panjang & valid.
    bool replaceChain(const std::string& serialized);

    const std::vector<Block>& chain() const { return chain_; }
    size_t height() const { return chain_.size(); }
    const Block& tip() const { return chain_.back(); }
    uint32_t difficulty() const { return difficultyBits_; }

    const UtxoSet& utxo() const { return utxo_; }
    uint64_t balanceOf(const std::string& address) const { return utxo_.balance(address); }

    std::string serialize() const;
    bool deserialize(const std::string& data);

private:
    void adjustDifficulty(uint64_t newTimestamp);
    // Validasi + terapkan satu blok ke UTXO (scratch). Return false bila invalid.
    bool applyBlock(const Block& b, UtxoSet& ut, uint64_t& supply) const;
    // Coba susun ulang rantai dari orphan pool setelah rantai berubah.
    bool tryReorgFromOrphans();
    bool tryForkFromOrphans();
    // Validasi & aktifkan rantai alternatif (alt = fork lengkap dari genesis).
    bool activateAlternative(std::vector<Block>& alt);

    std::vector<Block> chain_;
    uint64_t minedSupply_;
    uint32_t difficultyBits_;
    uint64_t totalWork_ = 0;
    UtxoSet utxo_;
    // Blok yang tidak menyambung ke tip — menunggu blok penghubungnya datang.
    std::vector<Block> orphans_;
};

// zyvor -> "X.YYYYYYYY ZYVC"
std::string formatCoin(uint64_t zyvor);

} // namespace zyv
