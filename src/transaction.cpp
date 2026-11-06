#include "transaction.h"
#include "sha256.h"

namespace zyv {

// txid = SHA256(SHA256(serializeUnsigned())) — dipanggil manual via helper.
// Fungsi-fungsi lama dipertahankan agar kompatibel dengan blockchain.cpp.
std::string computeTxId(const Transaction& tx) {
    return sha256(sha256(tx.serializeUnsigned()));
}

} // namespace zyv
