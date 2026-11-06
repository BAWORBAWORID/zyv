#pragma once
#include <sstream>
#include <string>
#include <vector>
#include <deque>
#include "transaction.h"

namespace zyv {

// Mempool — antrian transaksi valid yang belum masuk blok.
class Mempool {
public:
    bool add(const Transaction& t) {
        if (contains(t.id)) return false;
        txs_.push_back(t);
        return true;
    }

    bool contains(const std::string& txid) const {
        for (const auto& t : txs_) if (t.id == txid) return true;
        return false;
    }

    // Ambil maksimal `maxCount` transaksi (FIFO, tanpa menghapus).
    std::vector<Transaction> all() const {
        return std::vector<Transaction>(txs_.begin(), txs_.end());
    }

    // Hapus tx dengan txid yang ada di daftar `ids`.
    void remove(const std::vector<std::string>& ids) {
        for (auto it = txs_.begin(); it != txs_.end();) {
            bool match = false;
            for (const auto& id : ids) if (it->id == id) { match = true; break; }
            if (match) it = txs_.erase(it);
            else ++it;
        }
    }

    size_t size() const { return txs_.size(); }
    void clear() { txs_.clear(); }

    std::string serialize() const {
        std::string s;
        for (const auto& t : txs_) s += t.serialize() + "\n";
        return s;
    }

    void deserialize(const std::string& data) {
        txs_.clear();
        std::istringstream iss(data);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.empty()) continue;
            try { txs_.push_back(Transaction::deserialize(line)); } catch (...) {}
        }
    }

private:
    std::deque<Transaction> txs_;
};

} // namespace zyv
