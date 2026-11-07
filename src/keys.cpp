#include "keys.h"
#include "sha256.h"

#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/obj_mac.h>
#include <openssl/bn.h>
#include <openssl/ripemd.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/ecdh.h>
#include <openssl/kdf.h>

#include <algorithm>
#include <cstring>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <stdexcept>

namespace zyv {

static const char* BASE58_CHARS = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

static std::string toHex(const uint8_t* d, size_t n) {
    std::ostringstream o;
    o << std::hex << std::setfill('0');
    for (size_t i = 0; i < n; i++) o << std::setw(2) << int(d[i]);
    return o.str();
}

static std::vector<uint8_t> fromHex(const std::string& h) {
    std::vector<uint8_t> v;
    v.reserve(h.size() / 2);
    for (size_t i = 0; i + 1 < h.size(); i += 2) {
        v.push_back(uint8_t(std::stoi(h.substr(i, 2), nullptr, 16)));
    }
    return v;
}

std::string hash160(const std::string& data) {
    uint8_t sha[32];
    SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), sha);
    uint8_t rip[20];
    RIPEMD160(sha, 32, rip);
    return std::string(reinterpret_cast<char*>(rip), 20);
}

std::string base58Encode(const std::vector<uint8_t>& data) {
    if (data.empty()) return "";
    // Hitung jumlah leading zero bytes.
    size_t zeros = 0;
    while (zeros < data.size() && data[zeros] == 0) zeros++;

    BN_CTX* ctx = BN_CTX_new();
    BIGNUM* bn = BN_bin2bn(data.data(), (int)data.size(), nullptr);
    BIGNUM* base = BN_new();
    BIGNUM* div = BN_new();
    BIGNUM* rem = BN_new();
    BN_set_word(base, 58);

    std::string out;
    while (!BN_is_zero(bn)) {
        BN_div(div, rem, bn, base, ctx);
        out += BASE58_CHARS[BN_get_word(rem)];
        BN_copy(bn, div);
    }
    // Tambahkan '1' untuk tiap leading zero byte.
    for (size_t i = 0; i < zeros; i++) out += '1';

    std::reverse(out.begin(), out.end());

    BN_free(bn); BN_free(base); BN_free(div); BN_free(rem);
    BN_CTX_free(ctx);
    return out;
}

std::string base58CheckEncode(uint8_t version, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> data;
    data.push_back(version);
    data.insert(data.end(), payload.begin(), payload.end());

    // checksum = SHA256(SHA256(version + payload))[:4]
    uint8_t h1[32], h2[32];
    SHA256(data.data(), data.size(), h1);
    SHA256(h1, 32, h2);

    for (int i = 0; i < 4; i++) data.push_back(h2[i]);
    return base58Encode(data);
}

static std::string pubKeyHex(EC_KEY* key) {
    const EC_GROUP* grp = EC_KEY_get0_group(key);
    const EC_POINT* pt = EC_KEY_get0_public_key(key);
    size_t len = EC_POINT_point2oct(grp, pt, POINT_CONVERSION_COMPRESSED, nullptr, 0, nullptr);
    std::vector<uint8_t> buf(len);
    EC_POINT_point2oct(grp, pt, POINT_CONVERSION_COMPRESSED, buf.data(), len, nullptr);
    return toHex(buf.data(), buf.size());
}

KeyPair keyPairFromPrivate(const std::string& privHex) {
    KeyPair kp;
    kp.privateKey = privHex;

    EC_KEY* key = EC_KEY_new_by_curve_name(NID_secp256k1);
    if (!key) throw std::runtime_error("EC_KEY_new_by_curve_name failed");

    BIGNUM* priv = BN_new();
    BN_hex2bn(&priv, privHex.c_str());
    if (EC_KEY_set_private_key(key, priv) != 1) {
        BN_free(priv); EC_KEY_free(key);
        throw std::runtime_error("invalid private key");
    }
    BN_free(priv);

    const EC_GROUP* grp = EC_KEY_get0_group(key);
    EC_POINT* pub = EC_POINT_new(grp);
    EC_POINT_mul(grp, pub, EC_KEY_get0_private_key(key), nullptr, nullptr, nullptr);
    if (EC_KEY_set_public_key(key, pub) != 1) {
        EC_POINT_free(pub); EC_KEY_free(key);
        throw std::runtime_error("set public key failed");
    }
    EC_POINT_free(pub);

    kp.publicKey = pubKeyHex(key);
    kp.address = publicKeyToAddress(kp.publicKey);
    EC_KEY_free(key);
    return kp;
}

KeyPair generateKeyPair() {
    EC_KEY* key = EC_KEY_new_by_curve_name(NID_secp256k1);
    if (!key) throw std::runtime_error("EC_KEY_new_by_curve_name failed");
    if (EC_KEY_generate_key(key) != 1) {
        EC_KEY_free(key);
        throw std::runtime_error("EC_KEY_generate_key failed");
    }

    const BIGNUM* priv = EC_KEY_get0_private_key(key);
    char* privHex = BN_bn2hex(priv);
    KeyPair kp;
    kp.privateKey = privHex;
    OPENSSL_free(privHex);

    kp.publicKey = pubKeyHex(key);
    kp.address = publicKeyToAddress(kp.publicKey);
    EC_KEY_free(key);
    return kp;
}

std::string publicKeyToAddress(const std::string& pubkeyHex) {
    std::vector<uint8_t> pub = fromHex(pubkeyHex);
    std::string h = hash160(std::string(reinterpret_cast<char*>(pub.data()), pub.size()));
    std::vector<uint8_t> payload(h.begin(), h.end());
    return base58CheckEncode(0x1C, payload); // 0x1C = prefix mainnet ZYVC
}

std::string signMessage(const std::string& privHex, const std::string& msgHashHex) {
    std::vector<uint8_t> digest = fromHex(msgHashHex);
    if (digest.size() != 32) throw std::runtime_error("message hash must be 32 bytes");

    EC_KEY* key = EC_KEY_new_by_curve_name(NID_secp256k1);
    BIGNUM* priv = BN_new();
    BN_hex2bn(&priv, privHex.c_str());
    EC_KEY_set_private_key(key, priv);
    BN_free(priv);

    unsigned char sig[128];
    unsigned int siglen = sizeof(sig);
    if (ECDSA_sign(0, digest.data(), (int)digest.size(), sig, &siglen, key) != 1) {
        EC_KEY_free(key);
        throw std::runtime_error("ECDSA_sign failed");
    }
    EC_KEY_free(key);
    return toHex(sig, siglen);
}

bool verifySignature(const std::string& pubkeyHex, const std::string& msgHashHex,
                     const std::string& sigHex) {
    std::vector<uint8_t> digest = fromHex(msgHashHex);
    std::vector<uint8_t> sig = fromHex(sigHex);
    std::vector<uint8_t> pub = fromHex(pubkeyHex);
    if (digest.size() != 32) return false;

    EC_KEY* key = EC_KEY_new_by_curve_name(NID_secp256k1);
    if (!key) return false;
    const EC_GROUP* grp = EC_KEY_get0_group(key);
    EC_POINT* pt = EC_POINT_new(grp);
    if (EC_POINT_oct2point(grp, pt, pub.data(), pub.size(), nullptr) != 1) {
        EC_POINT_free(pt); EC_KEY_free(key);
        return false;
    }
    EC_KEY_set_public_key(key, pt);
    EC_POINT_free(pt);

    int ok = ECDSA_verify(0, digest.data(), (int)digest.size(),
                          sig.data(), (int)sig.size(), key);
    EC_KEY_free(key);
    return ok == 1;
}

// ============================================================================
// Enkripsi wallet: AES-256-GCM + PBKDF2-HMAC-SHA256
// File: "ZYVW1|" + hex(salt[16] + iv[12] + ciphertext + tag[16])
// ============================================================================
static const char WALLET_MAGIC[] = "ZYVW1|";

static bool deriveKey(const std::string& passphrase, const uint8_t* salt,
                      size_t saltLen, uint8_t out[32]) {
    return PKCS5_PBKDF2_HMAC(passphrase.data(), (int)passphrase.size(),
                             salt, (int)saltLen, 200000, EVP_sha256(), 32, out) == 1;
}

static std::string whToHex(const uint8_t* data, size_t len) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < len; i++) { s += d[data[i] >> 4]; s += d[data[i] & 15]; }
    return s;
}
static int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static bool whFromHex(const std::string& s, std::vector<uint8_t>& out) {
    if (s.size() % 2) return false;
    out.clear();
    for (size_t i = 0; i < s.size(); i += 2) {
        int hi = hexVal(s[i]), lo = hexVal(s[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(uint8_t((hi << 4) | lo));
    }
    return true;
}

bool encryptWalletFile(const std::string& path, const std::string& plaintext,
                       const std::string& passphrase) {
    uint8_t salt[16], iv[12];
    if (RAND_bytes(salt, sizeof(salt)) != 1 || RAND_bytes(iv, sizeof(iv)) != 1) return false;

    uint8_t key[32];
    if (!deriveKey(passphrase, salt, sizeof(salt), key)) return false;

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    bool ok = true;
    std::vector<uint8_t> ct(plaintext.size() + 16); // + tag
    int len = 0, ctLen = 0;
    if (ok && EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) ok = false;
    if (ok && EVP_EncryptInit_ex(ctx, nullptr, nullptr, key, iv) != 1) ok = false;
    if (ok && plaintext.size() &&
        EVP_EncryptUpdate(ctx, ct.data(), &len,
                          (const uint8_t*)plaintext.data(), (int)plaintext.size()) != 1) ok = false;
    ctLen = len;
    if (ok && EVP_EncryptFinal_ex(ctx, ct.data() + ctLen, &len) != 1) ok = false;
    ctLen += len;
    if (ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, ct.data() + ctLen) != 1) ok = false;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return false;

    std::vector<uint8_t> blob;
    blob.insert(blob.end(), salt, salt + sizeof(salt));
    blob.insert(blob.end(), iv, iv + sizeof(iv));
    blob.insert(blob.end(), ct.begin(), ct.begin() + ctLen + 16);

    std::ofstream f(path, std::ios::trunc);
    f << WALLET_MAGIC << whToHex(blob.data(), blob.size()) << "\n";
    return f.good();
}

bool decryptWalletFile(const std::string& path, std::string& plaintext,
                       const std::string& passphrase) {
    std::ifstream f(path);
    if (!f.good()) return false;
    std::string line;
    if (!std::getline(f, line)) return false;
    if (line.rfind(WALLET_MAGIC, 0) != 0) return false; // bukan format terenkripsi

    std::vector<uint8_t> blob;
    if (!whFromHex(line.substr(strlen(WALLET_MAGIC)), blob)) return false;
    if (blob.size() < 16 + 12 + 16) return false;

    uint8_t key[32];
    if (!deriveKey(passphrase, blob.data(), 16, key)) return false;
    const uint8_t* iv = blob.data() + 16;
    const uint8_t* ct = blob.data() + 28;
    int ctLen = int(blob.size() - 28 - 16);
    const uint8_t* tag = blob.data() + blob.size() - 16;

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    std::vector<uint8_t> pt(ctLen + 16);
    bool ok = true;
    int len = 0, ptLen = 0;
    if (ok && EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) ok = false;
    if (ok && EVP_DecryptInit_ex(ctx, nullptr, nullptr, key, iv) != 1) ok = false;
    if (ok && ctLen > 0 && EVP_DecryptUpdate(ctx, pt.data(), &len, ct, ctLen) != 1) ok = false;
    ptLen = len;
    if (ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, (void*)tag) != 1) ok = false;
    if (ok && EVP_DecryptFinal_ex(ctx, pt.data() + ptLen, &len) != 1) ok = false; // tag salah = passphrase salah
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return false;

    plaintext.assign(pt.begin(), pt.begin() + ptLen);
    return true;
}

// ============================================================================
// Enkripsi pesan on-chain (ECIES)
//   ECDH secp256k1 (ephemeral x recipient pubkey) -> HKDF-SHA256 -> AES-256-GCM
// Memo: "1|" + ephemeral_pubkey_hex + "|" + nonce_hex + "|" + ct_hex(+tag16)
// ============================================================================

bool isEncryptedMemo(const std::string& memo) {
    return memo.rfind("1|", 0) == 0;
}

// ECDH: privKey x pubKeyBytes -> 32-byte shared secret (X koordinat)
static bool ecdhShared(const std::string& privHex, const std::vector<uint8_t>& pubBytes,
                       uint8_t out[32]) {
    EC_KEY* key = EC_KEY_new_by_curve_name(NID_secp256k1);
    if (!key) return false;
    BIGNUM* priv = BN_new();
    BN_hex2bn(&priv, privHex.c_str());
    bool ok = EC_KEY_set_private_key(key, priv) == 1;
    const EC_GROUP* grp = ok ? EC_KEY_get0_group(key) : nullptr;
    EC_POINT* pubPt = nullptr;
    if (ok) {
        pubPt = EC_POINT_new(grp);
        ok = EC_POINT_oct2point(grp, pubPt, pubBytes.data(), pubBytes.size(), nullptr) == 1;
    }
    if (ok) ok = EC_KEY_set_public_key(key, pubPt) == 1;
    if (ok) {
        // ECDH_compute_key menghasilkan X koordinat shared point
        ok = ECDH_compute_key(out, 32, pubPt, key, nullptr) == 32;
    }
    if (pubPt) EC_POINT_free(pubPt);
    BN_free(priv);
    EC_KEY_free(key);
    return ok;
}

// HKDF-Extract+Expand sederhana (salt kosong, info "zyvor-msg-v1")
static void hkdfKey(const uint8_t* secret, size_t secretLen, uint8_t out[32]) {
    const std::string info = "zyvor-msg-v1";
    uint8_t prk[32];
    unsigned int prkLen = 0;
    // Extract: HMAC-SHA256(salt="", secret)
    HMAC(EVP_sha256(), nullptr, 0, secret, (int)secretLen, prk, &prkLen);
    // Expand: T(1) = HMAC(prk, info || 0x01)
    std::vector<uint8_t> data(info.begin(), info.end());
    data.push_back(1);
    unsigned int outLen = 0;
    HMAC(EVP_sha256(), prk, 32, data.data(), (int)data.size(), out, &outLen);
}

std::string encryptMessage(const std::string& recipientPubKeyHex,
                           const std::string& plaintext) {
    // 1. ephemeral keypair
    EC_KEY* eph = EC_KEY_new_by_curve_name(NID_secp256k1);
    if (!eph || EC_KEY_generate_key(eph) != 1) { if (eph) EC_KEY_free(eph); return ""; }
    const EC_GROUP* grp = EC_KEY_get0_group(eph);
    const EC_POINT* pt = EC_KEY_get0_public_key(eph);
    size_t pubLen = EC_POINT_point2oct(grp, pt, POINT_CONVERSION_COMPRESSED, nullptr, 0, nullptr);
    std::vector<uint8_t> ephPub(pubLen);
    EC_POINT_point2oct(grp, pt, POINT_CONVERSION_COMPRESSED, ephPub.data(), pubLen, nullptr);
    const BIGNUM* ephPriv = EC_KEY_get0_private_key(eph);
    char* ephPrivHexC = BN_bn2hex(ephPriv);
    std::string ephPrivHex = ephPrivHexC;
    OPENSSL_free(ephPrivHexC);
    EC_KEY_free(eph);

    // 2. ECDH dengan pubkey penerima
    std::vector<uint8_t> recipPub;
    if (!whFromHex(recipientPubKeyHex, recipPub) || recipPub.size() != 33) return "";
    if (recipPub.empty()) return "";
    uint8_t secret[32];
    if (!ecdhShared(ephPrivHex, recipPub, secret)) return "";

    // 3. HKDF -> AES key
    uint8_t aesKey[32];
    hkdfKey(secret, 32, aesKey);

    // 4. AES-256-GCM encrypt
    uint8_t iv[12];
    if (RAND_bytes(iv, sizeof(iv)) != 1) return "";
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return "";
    std::vector<uint8_t> ct(plaintext.size() + 16);
    int len = 0, ctLen = 0;
    bool ok = true;
    if (ok && EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) ok = false;
    if (ok && EVP_EncryptInit_ex(ctx, nullptr, nullptr, aesKey, iv) != 1) ok = false;
    if (ok && !plaintext.empty() &&
        EVP_EncryptUpdate(ctx, ct.data(), &len,
                          (const uint8_t*)plaintext.data(), (int)plaintext.size()) != 1) ok = false;
    ctLen = len;
    if (ok && EVP_EncryptFinal_ex(ctx, ct.data() + ctLen, &len) != 1) ok = false;
    ctLen += len;
    if (ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, ct.data() + ctLen) != 1) ok = false;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return "";

    // 5. format memo
    std::string memo = "1|" + whToHex(ephPub.data(), ephPub.size()) + "|" +
                       whToHex(iv, sizeof(iv)) + "|" +
                       whToHex(ct.data(), ctLen + 16);
    return memo;
}

std::string decryptMessage(const std::string& privHex,
                           const std::string& encMemo,
                           std::string& err) {
    err.clear();
    if (!isEncryptedMemo(encMemo)) { err = "bukan memo terenkripsi"; return ""; }
    // parse 3 field
    size_t p1 = encMemo.find('|');                 // after "1"
    size_t p2 = encMemo.find('|', p1 + 1);
    size_t p3 = encMemo.find('|', p2 + 1);
    if (p3 == std::string::npos) { err = "format salah"; return ""; }
    std::string ephPubHex = encMemo.substr(p1 + 1, p2 - p1 - 1);
    std::string ivHex = encMemo.substr(p2 + 1, p3 - p2 - 1);
    std::string ctHex = encMemo.substr(p3 + 1);

    std::vector<uint8_t> ephPub, iv, ct;
    if (!whFromHex(ephPubHex, ephPub) || ephPub.size() != 33) { err = "ephemeral pubkey salah"; return ""; }
    if (!whFromHex(ivHex, iv) || iv.size() != 12) { err = "nonce salah"; return ""; }
    if (!whFromHex(ctHex, ct) || ct.size() < 17) { err = "ciphertext salah"; return ""; }

    uint8_t secret[32];
    if (!ecdhShared(privHex, ephPub, secret)) { err = "ECDH gagal"; return ""; }
    uint8_t aesKey[32];
    hkdfKey(secret, 32, aesKey);

    const uint8_t* tag = ct.data() + ct.size() - 16;
    int ctLen = int(ct.size() - 16);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) { err = "ctx gagal"; return ""; }
    std::vector<uint8_t> plain(ctLen + 16);
    bool ok = true;
    int len = 0, ptLen = 0;
    if (ok && EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) ok = false;
    if (ok && EVP_DecryptInit_ex(ctx, nullptr, nullptr, aesKey, iv.data()) != 1) ok = false;
    if (ok && ctLen > 0 && EVP_DecryptUpdate(ctx, plain.data(), &len, ct.data(), ctLen) != 1) ok = false;
    ptLen = len;
    if (ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, (void*)tag) != 1) ok = false;
    if (ok && EVP_DecryptFinal_ex(ctx, plain.data() + ptLen, &len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        err = "dekripsi gagal (bukan penerima / pesan rusak)";
        return "";
    }
    EVP_CIPHER_CTX_free(ctx);
    return std::string(plain.begin(), plain.begin() + ptLen);
}

} // namespace zyv
