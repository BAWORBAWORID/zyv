#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include "transaction.h"

namespace zyv {

// Blok pada rantai Zyvor Coin.
struct Block {
    uint64_t index = 0;
    uint64_t timestamp = 0;
    std::string prevHash;
    std::string merkleRoot;
    uint64_t nonce = 0;
    uint32_t difficulty = 0;
    std::string hash;

    std::vector<Transaction> transactions;

    // Header yang di-hash untuk proof-of-work.
    std::string serializeHeader() const {
        return std::to_string(index) + "|" + std::to_string(timestamp) + "|" +
               prevHash + "|" + merkleRoot + "|" + std::to_string(nonce) + "|" +
               std::to_string(difficulty);
    }
};

// Hash blok = SHA-256(header).
std::string blockHash(const Block& b);

// Merkle root dari daftar hash transaksi.
std::string merkleRoot(const std::vector<std::string>& txHashes);

// Cek apakah hash memenuhi target kesulitan (N bit nol di depan).
bool meetsDifficulty(const std::string& hexHash, uint32_t difficultyBits);

// Serialisasi blok untuk storage/P2P (satu baris header + N baris tx).
std::string serializeBlock(const Block& b);
bool readBlock(std::istream& in, Block& b);

} // namespace zyv
