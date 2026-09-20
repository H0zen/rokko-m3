#pragma once

// The patcher's hashes are the tree's own (src/shared/Crypto), tested against the
// FIPS/RFC vectors and the captured goldens; this header keeps the patcher's small
// vocabulary -- digests as std::array, hex helpers -- over them.

#include "Crypto/Hex.h"
#include "Crypto/HmacSha1.h"
#include "Crypto/Sha1.h"
#include "Crypto/Sha256.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mangos::patcher {

using Sha256Digest = std::array<std::uint8_t, 32>;
using Sha1Digest = std::array<std::uint8_t, 20>;

inline Sha256Digest Sha256(const std::uint8_t* data, std::size_t len) {
    return MaNGOS::Crypto::Sha256(data, len);
}

inline Sha256Digest Sha256(const std::vector<std::uint8_t>& data) {
    return MaNGOS::Crypto::Sha256(data.data(), data.size());
}

inline Sha1Digest Sha1(const std::uint8_t* data, std::size_t len) {
    return MaNGOS::Crypto::Sha1(data, len);
}

inline Sha1Digest HmacSha1(const std::vector<std::uint8_t>& key,
                           const std::vector<std::uint8_t>& msg) {
    MaNGOS::Crypto::HmacSha1 mac(key.data(), key.size());
    mac.Update(msg.data(), msg.size());
    Sha1Digest out{};
    mac.Finish(out.data());
    return out;
}

inline std::string ToHex(const std::uint8_t* data, std::size_t len) {
    return MaNGOS::Crypto::ToHex(data, len);
}

template <std::size_t N>
std::string ToHex(const std::array<std::uint8_t, N>& d) {
    return ToHex(d.data(), d.size());
}

inline bool FromHex(const std::string& text, std::vector<std::uint8_t>& out) {
    return MaNGOS::Crypto::FromHex(text, out);
}

}  // namespace mangos::patcher
