// ============================================================================
// zyvcore.h — C API library mobile Zyvor (Android NDK / iOS, via Flutter FFI)
// ============================================================================
// Wrapper tipis di atas core Zyvor (keys.cpp, transaction.cpp, sha256.cpp).
// Flutter memanggil ini lewat dart:ffi (lihat mobile/flutter/README.md).
//   - generate wallet (keygen)
//   - sign transaction & serialize (POST /api/sendrawtx)
//   - encrypt/decrypt pesan (ECIES secp256k1 + AES-256-GCM)
// Semua fungsi mengembalikan string malloc'ed → caller wajib free via
// zyv_free(). Return "ERROR: ..." bila gagal.
// ============================================================================
#ifndef ZYVCORE_H
#define ZYVCORE_H

#ifdef __cplusplus
extern "C" {
#endif

// Beaskan string hasil zyv_* (wajib dipanggil caller)
void zyv_free(char* s);

// Generate keypair baru. Return JSON:
// {"address":"...","privkey":"...","pubkey":"..."}
char* zyv_keygen(void);

// Address dari pubkey hex. Return "..." atau "ERROR: ..."
char* zyv_address_from_pubkey(const char* pubkey_hex);

// Sign pesan dgn privkey hex → signature hex
char* zyv_sign(const char* privkey_hex, const char* message);

// Enkripsi pesan (plaintext UTF-8) ke pubkey penerima (hex)
// → format on-chain "1|<eph>|<nonce>|<ct>" (identik dgn keys.cpp)
char* zyv_encrypt(const char* recipient_pubkey_hex, const char* plaintext);

// Dekripsi memo on-chain dgn privkey penerima → plaintext, atau ""
char* zyv_decrypt(const char* privkey_hex, const char* encrypted_memo);

// Bangun & sign transaction → serialized (siap POST /api/sendrawtx)
// inputs_json : [{"txid":"..","vout":0}]  (pubkey diambil dari privkey)
// outputs_json: [{"address":"..","amount":10000}] (amount = unit terkecil)
// data        : memo on-chain (boleh NULL/kosong)
// privkey_hex : kunci pengirim (harus pemilik semua inputs)
char* zyv_build_tx(const char* inputs_json, const char* outputs_json,
                   const char* data, const char* privkey_hex);

// Info versi library
char* zyv_version(void);

#ifdef __cplusplus
}
#endif

#endif // ZYVCORE_H
