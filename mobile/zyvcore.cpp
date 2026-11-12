// ============================================================================
// zyvcore.cpp — implementasi C API library mobile Zyvor (Flutter FFI)
// ============================================================================
// Compile bersama: keys.cpp transaction.cpp sha256.cpp + OpenSSL (libcrypto)
// Contoh (host test):
//   g++ -std=c++17 -shared -fPIC -o libzyvcore.so zyvcore.cpp ../src/keys.cpp \
//       ../src/transaction.cpp ../src/sha256.cpp -I../src -lcrypto
// ============================================================================
#include "zyvcore.h"
#include "keys.h"
#include "transaction.h"
#include "blockchain.h"

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <sstream>
#include <vector>

using namespace zyv;

// ---------------------------------------------------------------- helpers
static char* dup_str(const std::string& s) {
    char* out = (char*)malloc(s.size() + 1);
    if (!out) return nullptr;
    memcpy(out, s.c_str(), s.size() + 1);
    return out;
}

// JSON mini-parser utk inputs/outputs (format terkendali dari Flutter side).
// inputs_json : [{"txid":"..","vout":0,"pubkey":".."}]
// outputs_json: [{"address":"..","amount":10000}]
static std::string jsonGetString(const std::string& obj, const std::string& key) {
    std::string pat = "\"" + key + "\"";
    size_t p = obj.find(pat);
    if (p == std::string::npos) return "";
    p = obj.find(':', p + pat.size());
    if (p == std::string::npos) return "";
    p = obj.find('"', p);
    if (p == std::string::npos) return "";
    size_t e = obj.find('"', p + 1);
    if (e == std::string::npos) return "";
    return obj.substr(p + 1, e - p - 1);
}
static uint64_t jsonGetNum(const std::string& obj, const std::string& key, uint64_t def = 0) {
    std::string pat = "\"" + key + "\"";
    size_t p = obj.find(pat);
    if (p == std::string::npos) return def;
    p = obj.find(':', p + pat.size());
    if (p == std::string::npos) return def;
    try { return std::stoull(obj.substr(p + 1)); } catch (...) { return def; }
}
// pisah objek JSON array sederhana "{...},{...}"
static std::vector<std::string> jsonSplitObjects(const std::string& arr) {
    std::vector<std::string> out;
    int depth = 0; size_t start = std::string::npos;
    for (size_t i = 0; i < arr.size(); i++) {
        char c = arr[i];
        if (c == '{') { if (depth == 0) start = i; depth++; }
        else if (c == '}') { depth--; if (depth == 0 && start != std::string::npos) { out.push_back(arr.substr(start, i - start + 1)); start = std::string::npos; } }
    }
    return out;
}

// ---------------------------------------------------------------- API
void zyv_free(char* s) { free(s); }

char* zyv_version(void) {
    return dup_str("zyvcore 1.0 (flutter ffi)");
}

char* zyv_keygen(void) {
    try {
        KeyPair kp = generateKeyPair();
        std::ostringstream o;
        o << "{\"address\":\"" << kp.address << "\","
          << "\"privkey\":\"" << kp.privateKey << "\","
          << "\"pubkey\":\"" << kp.publicKey << "\"}";
        return dup_str(o.str());
    } catch (const std::exception& e) {
        return dup_str(std::string("ERROR: ") + e.what());
    }
}

char* zyv_address_from_pubkey(const char* pubkey_hex) {
    if (!pubkey_hex) return dup_str("ERROR: null");
    try {
        return dup_str(publicKeyToAddress(pubkey_hex));
    } catch (const std::exception& e) {
        return dup_str(std::string("ERROR: ") + e.what());
    }
}

char* zyv_sign(const char* privkey_hex, const char* message) {
    if (!privkey_hex || !message) return dup_str("ERROR: null");
    try {
        return dup_str(signMessage(privkey_hex, message));
    } catch (const std::exception& e) {
        return dup_str(std::string("ERROR: ") + e.what());
    }
}

char* zyv_encrypt(const char* recipient_pubkey_hex, const char* plaintext) {
    if (!recipient_pubkey_hex || !plaintext) return dup_str("ERROR: null");
    try {
        std::string ct = encryptMessage(recipient_pubkey_hex, plaintext);
        if (ct.empty()) return dup_str("ERROR: enkripsi gagal");
        return dup_str(ct);
    } catch (const std::exception& e) {
        return dup_str(std::string("ERROR: ") + e.what());
    }
}

char* zyv_decrypt(const char* privkey_hex, const char* encrypted_memo) {
    if (!privkey_hex || !encrypted_memo) return dup_str("ERROR: null");
    try {
        std::string err;
        std::string pt = decryptMessage(privkey_hex, encrypted_memo, err);
        if (pt.empty()) return dup_str(("ERROR: " + (err.empty() ? "dekripsi gagal" : err)).c_str());
        return dup_str(pt);
    } catch (const std::exception& e) {
        return dup_str(std::string("ERROR: ") + e.what());
    }
}

char* zyv_build_tx(const char* inputs_json, const char* outputs_json,
                   const char* data, const char* privkey_hex) {
    if (!inputs_json || !outputs_json || !privkey_hex) return dup_str("ERROR: null");
    try {
        KeyPair kp = keyPairFromPrivate(privkey_hex);

        Transaction tx;
        for (const auto& obj : jsonSplitObjects(inputs_json)) {
            TxIn in;
            in.prevOut.txid = jsonGetString(obj, "txid");
            in.prevOut.index = (uint32_t)jsonGetNum(obj, "vout", 0);
            in.pubkey = kp.publicKey;
            tx.inputs.push_back(in);
        }
        if (tx.inputs.empty()) return dup_str("ERROR: tidak ada inputs");

        for (const auto& obj : jsonSplitObjects(outputs_json)) {
            TxOut out;
            out.amount = jsonGetNum(obj, "amount", 0);
            out.address = jsonGetString(obj, "address");
            if (out.address.empty()) return dup_str("ERROR: output tanpa address");
            tx.outputs.push_back(out);
        }
        if (tx.outputs.empty()) return dup_str("ERROR: tidak ada outputs");

        tx.timestamp = (uint64_t)time(nullptr);
        if (data && *data) tx.data = data;

        tx.id = computeTxId(tx);
        for (auto& in : tx.inputs) in.signature = signMessage(kp.privateKey, tx.id);

        return dup_str(tx.serialize());
    } catch (const std::exception& e) {
        return dup_str(std::string("ERROR: ") + e.what());
    }
}
