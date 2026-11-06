#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include "transaction.h"

namespace zyv {

struct UtxoEntry {
    uint64_t amount = 0;   // zyvor
    std::string address;
};

// UTXO set — model kepemilikan ala Bitcoin.
class UtxoSet {
public:
    void add(const std::string& txid, uint32_t index, uint64_t amount, const std::string& address) {
        utxos_[{txid, index}] = {amount, address};
    }

    bool remove(const OutPoint& op) {
        return utxos_.erase(op) > 0;
    }

    bool contains(const OutPoint& op) const {
        return utxos_.count(op) > 0;
    }

    const UtxoEntry* get(const OutPoint& op) const {
        auto it = utxos_.find(op);
        return it == utxos_.end() ? nullptr : &it->second;
    }

    uint64_t balance(const std::string& address) const {
        uint64_t sum = 0;
        for (const auto& kv : utxos_) {
            if (kv.second.address == address) sum += kv.second.amount;
        }
        return sum;
    }

    std::vector<std::pair<OutPoint, UtxoEntry>> allForAddress(const std::string& address) const {
        std::vector<std::pair<OutPoint, UtxoEntry>> res;
        for (const auto& kv : utxos_) {
            if (kv.second.address == address) res.push_back(kv);
        }
        return res;
    }

    size_t size() const { return utxos_.size(); }

private:
    std::map<OutPoint, UtxoEntry> utxos_;
};

} // namespace zyv
