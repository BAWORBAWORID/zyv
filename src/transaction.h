#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace zyv {

// ============================================================================
// Model transaksi UTXO (ala Bitcoin).
// ============================================================================

struct OutPoint {
    std::string txid; // 64 hex
    uint32_t index = 0;

    bool operator<(const OutPoint& o) const {
        if (txid != o.txid) return txid < o.txid;
        return index < o.index;
    }
    bool operator==(const OutPoint& o) const {
        return txid == o.txid && index == o.index;
    }
};

// Output: sejumlah zyvor terkunci ke alamat pemilik.
struct TxOut {
    uint64_t amount = 0;    // zyvor
    std::string address;
};

// Input: referensi ke output sebelumnya + bukti kepemilikan.
struct TxIn {
    OutPoint prevOut;
    std::string signature; // DER hex; tanda tangan atas txid
    std::string pubkey;    // compressed 33-byte hex
};

struct Transaction {
    std::string id; // txid = SHA256(SHA256(serializeUnsigned()))
    std::vector<TxIn> inputs;
    std::vector<TxOut> outputs;
    uint64_t timestamp = 0;
    // Data bebas (OP_RETURN-style), maks MAX_TX_DATA_BYTES byte, hex.
    // Tidak boleh menempel ke UTXO (unspendable), hanya memo/on-chain note.
    std::string data;

    bool isCoinbase() const { return inputs.empty(); }

    // Serialisasi untuk menghitung txid (tidak termasuk signature).
    std::string serializeUnsigned() const {
        std::string s;
        for (const auto& in : inputs) {
            s += in.prevOut.txid + ":" + std::to_string(in.prevOut.index) + "|" +
                 in.pubkey + "|";
        }
        for (const auto& out : outputs) {
            s += std::to_string(out.amount) + ":" + out.address + "|";
        }
        s += std::to_string(timestamp);
        if (!data.empty()) {
            std::string d = data;
            std::string esc;
            for (char c : d) { if (c == '|') esc += "%7C"; else if (c == '%') esc += "%25"; else esc += c; }
            s += "|" + esc;
        }
        return s;
    }

    uint64_t totalOutput() const {
        uint64_t s = 0;
        for (const auto& o : outputs) s += o.amount;
        return s;
    }

    // Serialisasi penuh (dengan signature) untuk mempool/wire.
    std::string serialize() const {
        std::string s = id + "|" + std::to_string(inputs.size());
        for (const auto& in : inputs) {
            s += "|" + in.prevOut.txid + "|" + std::to_string(in.prevOut.index) +
                 "|" + in.pubkey + "|" + in.signature;
        }
        s += "|" + std::to_string(outputs.size());
        for (const auto& out : outputs) {
            s += "|" + std::to_string(out.amount) + "|" + out.address;
        }
        s += "|" + std::to_string(timestamp);
        if (!data.empty()) {
            std::string d = data;
            std::string esc;
            for (char c : d) { if (c == '|') esc += "%7C"; else if (c == '%') esc += "%25"; else esc += c; }
            s += "|" + esc;
        }
        return s;
    }

    static Transaction deserialize(const std::string& line) {
        Transaction tx;
        size_t pos = 0;
        auto next = [&]() {
            size_t p = line.find('|', pos);
            std::string s = (p == std::string::npos) ? line.substr(pos) : line.substr(pos, p - pos);
            pos = (p == std::string::npos) ? line.size() : p + 1;
            return s;
        };
        tx.id = next();
        size_t nIn = std::stoul(next());
        for (size_t i = 0; i < nIn; i++) {
            TxIn in;
            in.prevOut.txid = next();
            in.prevOut.index = (uint32_t)std::stoul(next());
            in.pubkey = next();
            in.signature = next();
            tx.inputs.push_back(in);
        }
        size_t nOut = std::stoul(next());
        for (size_t i = 0; i < nOut; i++) {
            TxOut out;
            out.amount = std::stoull(next());
            out.address = next();
            tx.outputs.push_back(out);
        }
        tx.timestamp = std::stoull(next());
        // field opsional (backward-compatible): data (pipe di-escape %7C)
        if (pos < line.size()) {
            std::string d = next();
            std::string unesc;
            for (size_t i = 0; i < d.size(); i++) {
                if (d[i] == '%' && i + 2 < d.size() && d[i+1] == '7' && d[i+2] == 'C') { unesc += '|'; i += 2; }
                else if (d[i] == '%' && i + 2 < d.size() && d[i+1] == '2' && d[i+2] == '5') { unesc += '%'; i += 2; }
                else unesc += d[i];
            }
            tx.data = unesc;
        }
        return tx;
    }
};

} // namespace zyv
