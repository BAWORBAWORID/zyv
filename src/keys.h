#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace zyv {

// ============================================================================
// Kriptografi Zyvor Coin: kunci ECDSA (secp256k1), alamat, tanda tangan.
// ============================================================================

struct KeyPair {
    std::string privateKey; // 32-byte hex
    std::string publicKey;  // compressed 33-byte hex
    std::string address;    // base58check(RIPEMD160(SHA256(pubkey)))
};

// Buat pasangan kunci baru (secp256k1).
KeyPair generateKeyPair();

// Muat kunci dari private key hex (derives pubkey + address).
KeyPair keyPairFromPrivate(const std::string& privHex);

// Address dari public key (compressed 33-byte hex).
std::string publicKeyToAddress(const std::string& pubkeyHex);

// Tanda tangani 32-byte message hash; hasilnya DER signature hex.
std::string signMessage(const std::string& privHex, const std::string& msgHashHex);

// Verifikasi signature DER (hex) atas msgHash (hex) dengan pubkey (hex).
bool verifySignature(const std::string& pubkeyHex, const std::string& msgHashHex,
                     const std::string& sigHex);

// hash160 = RIPEMD160(SHA256(data)) -> 20 byte hex.
std::string hash160(const std::string& data);

// Encode base58 (tanpa checksum).
std::string base58Encode(const std::vector<uint8_t>& data);

// base58check(versionByte + payload).
std::string base58CheckEncode(uint8_t version, const std::vector<uint8_t>& payload);

// ============================================================================
// Enkripsi wallet — AES-256-GCM + PBKDF2 (OpenSSL).
// Format file: "ZYVW1|" + base64(salt[16] | iv[12] | ciphertext+tag[16]).
// ============================================================================
bool encryptWalletFile(const std::string& path, const std::string& plaintext,
                       const std::string& passphrase);
bool decryptWalletFile(const std::string& path, std::string& plaintext,
                       const std::string& passphrase);

// ============================================================================
// Enkripsi pesan on-chain (ECIES: ECDH secp256k1 + AES-256-GCM + HKDF-SHA256).
// Format memo terenkripsi:
//   "1|<ephemeral_pubkey_hex 66>|<nonce_hex 24>|<ciphertext+tag hex>"
// Hanya pemegang private key penerima yang bisa dekrip.
// ============================================================================
std::string encryptMessage(const std::string& recipientPubKeyHex,
                           const std::string& plaintext);
// privHex = private key penerima. Return plaintext asli, atau string kosong
// (dengan err diisi) bila gagal.
std::string decryptMessage(const std::string& privHex,
                           const std::string& encMemo,
                           std::string& err);
// Cek apakah memo berformat terenkripsi ("1|" prefix).
bool isEncryptedMemo(const std::string& memo);

} // namespace zyv
