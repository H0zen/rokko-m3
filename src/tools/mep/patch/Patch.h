#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ClientFile.h"
#include "Targets.h"

namespace mangos::patcher {

enum class SiteState {
    Stock,     // exactly the bytes the stock client ships
    Applied,   // exactly the bytes this patcher writes
    Foreign,   // present, but neither — someone else edited it
    Missing,   // the address or the blob is not in this image
};

struct SiteReport {
    std::string name;
    SiteState state = SiteState::Missing;
    std::uint64_t va = 0;
    std::size_t file_offset = 0;
    std::string detail;
};

struct ClientReport {
    const Target* target = nullptr;
    std::string machine;
    std::uint64_t image_base = 0;
    std::string sha256;
    bool sha256_is_stock = false;

    SiteReport modulus;
    SiteReport digest;
    SiteReport launcher;
    std::vector<SiteReport> forbidden;

    // A forbidden site that is no longer stock: the client collapses both
    // streams onto slot 0 and cannot exercise the dual-stream protocol.
    bool HasForbiddenEdits() const;
};

struct ApplyOptions {
    std::vector<std::uint8_t> modulus_le;  // 256 bytes, empty = leave alone
    std::vector<std::uint8_t> digest20;    // 20 bytes, empty = leave alone
    bool launcher = true;
    bool backup = true;
    bool dry_run = false;
    // Patch an image that is neither the stock client nor one this tool patched
    // before. Its layout is then a guess.
    bool allow_modified = false;
};

struct ApplyResult {
    bool ok = false;
    bool changed = false;
    std::vector<std::string> actions;
    std::string error;
};

ClientReport Inspect(const ClientFile& file);

ApplyResult Apply(ClientFile& file, const ClientReport& report, const ApplyOptions& options);

const char* SiteStateName(SiteState state);

// HMAC-SHA1(SEED64, auth73 || auth73) — what DIGEST20 must hold for a custom
// 73-byte redirect auth blob to verify.
bool DigestForAuthBlob(const std::vector<std::uint8_t>& auth73,
                       std::vector<std::uint8_t>& out, std::string& error);

}  // namespace mangos::patcher
