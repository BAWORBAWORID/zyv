#include "blockchain.h"
#include "sha256.h"
#include "keys.h"
#include <sstream>
#include <iostream>
#include <iomanip>
#include <set>
#include <stdexcept>

namespace zyv {

Blockchain::Blockchain() : minedSupply_(0), difficultyBits_(INITIAL_DIFFICULTY_BITS) {}

// Work per blok = 2^difficulty (clamp biar tidak overflow; cukup utk demo).
static uint64_t blockWork(uint32_t difficultyBits) {
    if (difficultyBits >= 63) return ~0ull;
    return 1ull << difficultyBits;
}

void Blockchain::createGenesis() {
    if (!chain_.empty()) return;

    Block g;
    g.index = 0;
    g.timestamp = GENESIS_TIMESTAMP;
    g.prevHash = GENESIS_PREV_HASH;
    g.difficulty = INITIAL_DIFFICULTY_BITS;

    Transaction coinbase; // coinbase: tanpa input
    coinbase.outputs.push_back({INITIAL_REWARD_ZYVOR, GENESIS_ADDRESS});
    coinbase.timestamp = GENESIS_TIMESTAMP;
    coinbase.id = computeTxId(coinbase);

    g.transactions.push_back(coinbase);
    g.merkleRoot = merkleRoot({coinbase.id});

    g.nonce = 0;
    g.hash = blockHash(g);
    while (!meetsDifficulty(g.hash, g.difficulty)) {
        ++g.nonce;
        g.hash = blockHash(g);
    }

    chain_.push_back(g);
    totalWork_ = blockWork(g.difficulty);
    minedSupply_ = INITIAL_REWARD_ZYVOR;
    utxo_.add(coinbase.id, 0, INITIAL_REWARD_ZYVOR, GENESIS_ADDRESS);
}

uint64_t Blockchain::rewardForHeight(uint64_t height) const {
    int halvings = halvingCount(height);
    return INITIAL_REWARD_ZYVOR >> halvings;
}

void Blockchain::adjustDifficulty(uint64_t newTimestamp) {
    if (chain_.size() <= 1) return;
    if (chain_.size() % DIFFICULTY_ADJUST_INTERVAL != 0) return;

    const Block& start = chain_[chain_.size() - DIFFICULTY_ADJUST_INTERVAL];
    uint64_t expected = DIFFICULTY_ADJUST_INTERVAL * TARGET_BLOCK_TIME_SEC;
    uint64_t actual = newTimestamp - start.timestamp;

    if (actual < expected / 2 && difficultyBits_ < 64) {
        ++difficultyBits_;
    } else if (actual > expected * 2 && difficultyBits_ > 1) {
        --difficultyBits_;
    }
}

bool Blockchain::validateTx(const Transaction& tx, const UtxoSet& ut) const {
    if (tx.inputs.empty()) return false; // coinbase tidak masuk mempool/validasi tx biasa

    // txid harus cocok dengan isi (anti tamper).
    if (computeTxId(tx) != tx.id) return false;

    uint64_t inSum = 0;
    std::set<OutPoint> seen;
    for (const auto& in : tx.inputs) {
        // no double-spend di dalam satu tx
        if (seen.count(in.prevOut)) return false;
        seen.insert(in.prevOut);

        const UtxoEntry* e = ut.get(in.prevOut);
        if (!e) return false; // UTXO tidak ada / sudah terpakai

        // pemilik UTXO harus sesuai dengan public key yang menandatangani
        if (e->address != publicKeyToAddress(in.pubkey)) return false;

        // verifikasi tanda tangan ECDSA atas txid
        if (!verifySignature(in.pubkey, tx.id, in.signature)) return false;

        inSum += e->amount;
    }
    // output tidak boleh melebihi input (fee >= 0)
    if (inSum < tx.totalOutput()) return false;
    return true;
}

bool Blockchain::applyBlock(const Block& b, UtxoSet& ut, uint64_t& supply) const {
    if (b.transactions.empty()) return false;

    const Transaction& cb = b.transactions[0];
    if (!cb.isCoinbase()) return false;
    if (cb.outputs.size() != 1) return false;

    uint64_t reward = rewardForHeight(b.index);
    uint64_t mintable = MAX_SUPPLY_ZYVOR - supply;
    uint64_t allowedReward = reward > mintable ? mintable : reward;

    uint64_t fees = 0;
    std::set<OutPoint> spent;
    for (size_t i = 1; i < b.transactions.size(); i++) {
        const Transaction& tx = b.transactions[i];
        if (!validateTx(tx, ut)) return false;
        uint64_t inSum = 0;
        for (const auto& in : tx.inputs) {
            if (spent.count(in.prevOut)) return false; // double-spend dalam blok
            spent.insert(in.prevOut);
            const UtxoEntry* e = ut.get(in.prevOut);
            inSum += e->amount;
        }
        fees += (inSum - tx.totalOutput());
    }

    // coinbase harus tepat = reward (terpotong supply) + total fee.
    if (cb.outputs[0].amount != allowedReward + fees) return false;

    // Terapkan: belanjakan input, ciptakan output.
    for (size_t i = 1; i < b.transactions.size(); i++) {
        const Transaction& tx = b.transactions[i];
        for (const auto& in : tx.inputs) ut.remove(in.prevOut);
        for (size_t o = 0; o < tx.outputs.size(); o++) {
            ut.add(tx.id, (uint32_t)o, tx.outputs[o].amount, tx.outputs[o].address);
        }
    }
    ut.add(cb.id, 0, cb.outputs[0].amount, cb.outputs[0].address);

    supply += allowedReward; // koin baru = reward (bukan fee)
    return true;
}

bool Blockchain::validateChain() {
    if (chain_.empty()) return false;
    UtxoSet ut;
    uint64_t supply = 0;
    uint64_t work = 0;
    for (size_t i = 0; i < chain_.size(); i++) {
        const Block& b = chain_[i];
        if (blockHash(b) != b.hash) return false;
        if (!meetsDifficulty(b.hash, b.difficulty)) return false;
        if (i > 0 && b.prevHash != chain_[i - 1].hash) return false;

        std::vector<std::string> hashes;
        for (const auto& t : b.transactions) hashes.push_back(t.id);
        if (merkleRoot(hashes) != b.merkleRoot) return false;

        if (!applyBlock(b, ut, supply)) return false;
        work += blockWork(b.difficulty);
    }
    utxo_ = ut;
    minedSupply_ = supply;
    totalWork_ = work;
    return true;
}

bool Blockchain::addBlock(const Block& b) {
    if (chain_.empty()) return false;
    if (blockHash(b) != b.hash) return false;
    if (!meetsDifficulty(b.hash, b.difficulty)) return false;

    std::vector<std::string> hashes;
    for (const auto& t : b.transactions) hashes.push_back(t.id);

    const Block& tip = chain_.back();

    // Kasus 1: blok menyambung ke tip -> extend.
    if (b.prevHash == tip.hash && b.index == tip.index + 1) {
        UtxoSet scratch = utxo_;
        uint64_t supply = minedSupply_;
        if (!applyBlock(b, scratch, supply)) return false;
        utxo_ = scratch;
        minedSupply_ = supply;
        chain_.push_back(b);
        totalWork_ += blockWork(b.difficulty);
        adjustDifficulty(b.timestamp);
        tryReorgFromOrphans(); // blok yatim mungkin kini menyambung
        return true;
    }

    // Kasus 2: duplikat blok yang sudah ada -> abaikan.
    for (const auto& c : chain_) if (c.hash == b.hash) return false;

    // Kasus 3: fork — blok menyambung ke blok TENGAH rantai.
    for (const auto& o : orphans_) if (o.hash == b.hash) return false;
    for (size_t i = 0; i < chain_.size(); i++) {
        if (chain_[i].hash == b.prevHash) {
            // cabang alternatif = rantai[0..i] + b + orphan berikutnya
            std::vector<Block> alt(chain_.begin(), chain_.begin() + i + 1);
            alt.push_back(b);
            // tarik blok yatim yang menyambung ke cabang ini (pakai copy —
            // hanya commit bila reorg sukses)
            bool grew = true;
            auto pool = orphans_;
            while (grew) {
                grew = false;
                for (auto it = pool.begin(); it != pool.end(); ++it) {
                    if (it->prevHash == alt.back().hash) {
                        alt.push_back(*it);
                        pool.erase(it);
                        grew = true;
                        break;
                    }
                }
            }
            if (activateAlternative(alt)) {
                orphans_ = pool;
                return true;
            }
            orphans_.push_back(b);
            return false;
        }
    }

    // Kasus 4: tidak menyambung ke tip -> simpan sebagai orphan,
    // lalu coba bangun cabang fork dari blok tengah rantai memakai orphan pool.
    for (const auto& o : orphans_) if (o.hash == b.hash) return false;
    if (orphans_.size() < 500) orphans_.push_back(b);
    return tryForkFromOrphans();
}

bool Blockchain::activateAlternative(std::vector<Block>& alt) {
    // Validasi rantai alternatif penuh dengan copy state.
    Blockchain tmp;
    tmp.chain_ = alt;
    if (!tmp.validateChain()) return false;

    if (tmp.totalWork_ <= totalWork_) return false; // hanya jika work lebih besar
    *this = tmp;
    return true;
}

// Bangun cabang alternatif terbaik dari orphan pool: cari orphan yang menyambung
// ke blok TENGAH rantai, ekstend dengan orphan lain, aktifkan bila total work lebih besar.
bool Blockchain::tryForkFromOrphans() {
    bool reorged = false;
    for (size_t i = chain_.size(); i-- > 1;) {
        for (const auto& o : orphans_) {
            if (o.prevHash != chain_[i].hash) continue;
            std::vector<Block> alt(chain_.begin(), chain_.begin() + i + 1);
            alt.push_back(o);
            // tarik rantai orphan yang menyambung
            bool grew = true;
            auto pool = orphans_;
            while (grew) {
                grew = false;
                for (auto it = pool.begin(); it != pool.end(); ++it) {
                    if (it->prevHash == alt.back().hash) {
                        alt.push_back(*it);
                        pool.erase(it);
                        grew = true;
                        break;
                    }
                }
            }

            if (activateAlternative(alt)) {
                orphans_ = pool;
                reorged = true;
                i = chain_.size(); // mulai ulang dari tip baru
                break;
            }
        }
    }
    return reorged;
}

bool Blockchain::tryReorgFromOrphans() {
    if (orphans_.empty()) return false;
    // Coba semua kombinasi yang menyambung dari tip (greedy, cukup utk demo).
    bool changed = false;
    bool grew = true;
    std::vector<Block> alt = chain_;
    while (grew) {
        grew = false;
        for (auto it = orphans_.begin(); it != orphans_.end(); ++it) {
            if (it->prevHash == alt.back().hash) {
                alt.push_back(*it);
                orphans_.erase(it);
                grew = true;
                changed = true;
                break;
            }
        }
    }
    if (!changed) return false;
    return activateAlternative(alt);
}

bool Blockchain::replaceChain(const std::string& serialized) {
    Blockchain tmp;
    if (!tmp.deserialize(serialized)) return false;
    if (!tmp.validateChain()) return false;
    if (tmp.height() <= chain_.size()) return false; // hanya terima yang lebih panjang
    *this = tmp;
    return true;
}

const Block& Blockchain::mineBlock(const std::vector<Transaction>& txs,
                                   const std::string& minerAddress) {
    if (chain_.empty()) createGenesis();

    const Block& prev = chain_.back();
    Block b;
    b.index = prev.index + 1;
    b.timestamp = GENESIS_TIMESTAMP + uint64_t(chain_.size()) * TARGET_BLOCK_TIME_SEC;
    b.prevHash = prev.hash;
    b.difficulty = difficultyBits_;

    // Coinbase (reward + fee).
    uint64_t reward = rewardForHeight(b.index);
    uint64_t mintable = MAX_SUPPLY_ZYVOR - minedSupply_;
    if (reward > mintable) reward = mintable;

    uint64_t fees = 0;
    UtxoSet scratch = utxo_; // copy
    std::vector<Transaction> accepted;
    for (const auto& tx : txs) {
        if (!validateTx(tx, scratch)) continue; // lewati tx tak valid
        uint64_t inSum = 0;
        for (const auto& in : tx.inputs) {
            const UtxoEntry* e = scratch.get(in.prevOut);
            inSum += e->amount;
            scratch.remove(in.prevOut);
        }
        fees += (inSum - tx.totalOutput());
        for (size_t o = 0; o < tx.outputs.size(); o++) {
            scratch.add(tx.id, (uint32_t)o, tx.outputs[o].amount, tx.outputs[o].address);
        }
        accepted.push_back(tx);
    }

    Transaction coinbase;
    coinbase.outputs.push_back({reward + fees, minerAddress.empty() ? GENESIS_ADDRESS : minerAddress});
    coinbase.timestamp = b.timestamp;
    coinbase.id = computeTxId(coinbase);

    b.transactions.push_back(coinbase);
    for (const auto& tx : accepted) b.transactions.push_back(tx);

    std::vector<std::string> hashes;
    for (const auto& t : b.transactions) hashes.push_back(t.id);
    b.merkleRoot = merkleRoot(hashes);

    b.nonce = 0;
    b.hash = blockHash(b);
    while (!meetsDifficulty(b.hash, b.difficulty)) {
        ++b.nonce;
        b.hash = blockHash(b);
    }

    // Commit UTXO + supply.
    utxo_ = scratch;
    utxo_.add(coinbase.id, 0, reward + fees, coinbase.outputs[0].address);
    minedSupply_ += reward;
    chain_.push_back(b);
    totalWork_ += blockWork(b.difficulty);
    adjustDifficulty(b.timestamp);
    return chain_.back();
}

std::string Blockchain::serialize() const {
    std::ostringstream oss;
    oss << difficultyBits_ << "\n";
    oss << minedSupply_ << "\n";
    oss << chain_.size() << "\n";
    for (const auto& b : chain_) {
        oss << serializeBlock(b);
    }
    return oss.str();
}

bool Blockchain::deserialize(const std::string& data) {
    std::istringstream iss(data);
    std::string line;
    if (!std::getline(iss, line)) return false;
    difficultyBits_ = uint32_t(std::stoul(line));
    if (!std::getline(iss, line)) return false;
    minedSupply_ = std::stoull(line);
    if (!std::getline(iss, line)) return false;
    size_t count = std::stoul(line);

    chain_.clear();
    for (size_t i = 0; i < count; i++) {
        Block b;
        if (!readBlock(iss, b)) return false;
        chain_.push_back(b);
    }
    return true;
}

std::string formatCoin(uint64_t zyvor) {
    uint64_t whole = zyvor / SATOSHIS_PER_COIN;
    uint64_t frac = zyvor % SATOSHIS_PER_COIN;
    std::ostringstream oss;
    oss << whole << "." << std::setw(8) << std::setfill('0') << frac;
    return oss.str();
}

} // namespace zyv
