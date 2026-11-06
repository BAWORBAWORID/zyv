#include "block.h"
#include "sha256.h"
#include <sstream>
#include <stdexcept>

namespace zyv {

std::string blockHash(const Block& b) {
    return sha256(b.serializeHeader());
}

std::string merkleRoot(const std::vector<std::string>& hashes) {
    if (hashes.empty()) return std::string(64, '0');
    std::vector<std::string> layer = hashes;
    while (layer.size() > 1) {
        if (layer.size() % 2 == 1) layer.push_back(layer.back());
        std::vector<std::string> next;
        for (size_t i = 0; i < layer.size(); i += 2) {
            next.push_back(sha256(layer[i] + layer[i + 1]));
        }
        layer = next;
    }
    return layer[0];
}

bool meetsDifficulty(const std::string& hexHash, uint32_t difficultyBits) {
    if (hexHash.size() != 64) return false;
    uint32_t bits = difficultyBits;
    size_t i = 0;
    while (bits >= 8 && i < hexHash.size()) {
        if (hexHash[i] != '0') return false;
        ++i;
        bits -= 8;
    }
    if (i >= hexHash.size()) return bits == 0;
    static const char* NIB = "0123456789abcdef";
    int val = -1;
    for (int k = 0; k < 16; k++) {
        if (NIB[k] == hexHash[i]) { val = k; break; }
    }
    if (val < 0) return false;
    return (val >> (4 - bits)) == 0;
}

std::string serializeBlock(const Block& b) {
    std::ostringstream oss;
    oss << b.index << "|" << b.timestamp << "|" << b.prevHash << "|" << b.merkleRoot
        << "|" << b.nonce << "|" << b.difficulty << "|" << b.hash << "|"
        << b.transactions.size() << "\n";
    for (const auto& t : b.transactions) {
        oss << t.serialize() << "\n";
    }
    return oss.str();
}

bool readBlock(std::istream& in, Block& b) {
    std::string line;
    if (!std::getline(in, line)) return false;
    if (line.empty()) return false;
    // parse header: 8 field dipisah '|'
    std::vector<std::string> f;
    std::string cur;
    for (char c : line) { if (c == '|') { f.push_back(cur); cur.clear(); } else cur += c; }
    f.push_back(cur);
    if (f.size() != 8) return false;
    try {
        b.index = std::stoull(f[0]);
        b.timestamp = std::stoull(f[1]);
        b.prevHash = f[2];
        b.merkleRoot = f[3];
        b.nonce = std::stoull(f[4]);
        b.difficulty = uint32_t(std::stoul(f[5]));
        b.hash = f[6];
        size_t txCount = std::stoul(f[7]);
        b.transactions.clear();
        for (size_t i = 0; i < txCount; i++) {
            if (!std::getline(in, line)) return false;
            b.transactions.push_back(Transaction::deserialize(line));
        }
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace zyv
