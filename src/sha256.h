#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

// SHA-256 digest utilities.
// sha256(str) returns lowercase hex of the digest.
std::string sha256(const std::string& input);
std::string sha256(const uint8_t* data, size_t len);

// Hex helpers.
std::string toHex(const uint8_t* data, size_t len);
std::vector<uint8_t> fromHex(const std::string& hex);
