#pragma once
#include <cstdint>
#include <string>

// ============================================================================
// Zyvor Coin (ZYVC) — parameter jaringan / konsensus
// ============================================================================
namespace zyv {

// --- Identitas ---
inline const std::string COIN_NAME = "Zyvor Coin";
inline const std::string TICKER    = "ZYVC";
inline const std::string SYMBOL    = "\xC6\xB5"; // Ƶ
inline const std::string CREATOR   = "Zyvor";

// --- Supply ---
constexpr uint64_t MAX_SUPPLY          = 21000000;                    // 21 juta ZYVC
constexpr uint64_t SATOSHIS_PER_COIN   = 100000000;                   // 1 ZYVC = 100 juta zyvor
constexpr uint64_t MAX_SUPPLY_ZYVOR    = MAX_SUPPLY * SATOSHIS_PER_COIN; // 2.1e15 zyvor

// --- Mekanisme inti ---
constexpr uint64_t TARGET_BLOCK_TIME_SEC = 600;         // 10 menit / blok
constexpr uint64_t HALVING_INTERVAL      = 210000;      // blok per halving (~4 tahun)
constexpr uint64_t INITIAL_REWARD_ZYVOR  = 50ull * SATOSHIS_PER_COIN; // 50 ZYVC = 5e9 zyvor
constexpr uint64_t DIFFICULTY_ADJUST_INTERVAL = 2016;   // blok (~2 minggu)

// Fee jaringan minimum (zyvor per transaksi). 10000 zyvor = 0.0001 ZYVC.
constexpr uint64_t MIN_TX_FEE = 10000;
// OP_RETURN-style: data bebas maksimal 80 byte (hex-encode → 160 char) per transaksi.
constexpr size_t MAX_TX_DATA_BYTES = 80;

// Seed node utama (DNS) — bootstrap peer untuk node baru.
inline const std::string DEFAULT_SEED_HOST = "bore.pub";
constexpr uint16_t DEFAULT_SEED_PORT = 5315;
inline const std::string DEFAULT_SEED_NODE = DEFAULT_SEED_HOST + ":" + std::to_string(DEFAULT_SEED_PORT);

// --- Genesis ---
// 1 Januari 2027 00:00:00 UTC (launch Zyvor Coin)
constexpr uint64_t GENESIS_TIMESTAMP = 1798761600ull;
inline const std::string GENESIS_PREV_HASH = "0000000000000000000000000000000000000000000000000000000000000000";
inline const std::string GENESIS_ADDRESS   = "GENESIS-ZYVOR";

// Tahun di mana koin terakhir di-mine (launch + 131 tahun)
inline const int LAST_COIN_YEAR = 2027 + 131; // 2158

// Kesulitan awal (jumlah bit nol di awal hash). Kecil agar demo cepat.
constexpr uint32_t INITIAL_DIFFICULTY_BITS = 4;

} // namespace zyv
