#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "PeImage.h"

namespace mangos::patcher {

// A code site the patcher either rewrites or refuses to touch. `expect` is the
// stock byte run at `va`; `patch` (when non-empty) replaces its first bytes.
struct CodeSite {
    std::string name;
    std::uint64_t va = 0;
    std::vector<std::uint8_t> expect;
    std::vector<std::uint8_t> patch;
    // Some sites are known by file offset rather than by address: the
    // MaNGOSPatcher table (mangosthree/tools) records what it rewrites that
    // way. When set, `file_offset` locates the site and `va` is derived from
    // the section table for the report.
    bool by_file_offset = false;
    std::size_t file_offset = 0;
};

// A blob located by its stock content, then cross-checked against the virtual
// address the disassembly cites.
struct DataSite {
    std::string name;
    std::uint64_t expect_va = 0;
    std::vector<std::uint8_t> stock;
};

struct Target {
    std::string label;
    Machine machine = Machine::Unknown;
    std::string stock_sha256;

    DataSite modulus;   // connection-redirect RSA modulus, 256 B little-endian
    DataSite seed;      // HMAC key SEED64
    DataSite digest;    // DIGEST20, HMAC-A of the stock auth blob
    CodeSite launcher;  // manifest / Launcher.exe check

    // Sites that must stay stock. A client with these rewritten is a
    // MaNGOSPatcher client: the recv gate is dead and every opcode is forced
    // onto stream 0, which disables the dual-stream architecture.
    std::vector<CodeSite> forbidden;
    // False until the MaNGOSPatcher edit sites of this image have been verified.
    // Without them a collapsed-stream client cannot be told from a stock one, so
    // the tool refuses to patch the image at all rather than claim a check it
    // does not make.
    bool forbidden_sites_known = false;
};

const std::vector<Target>& KnownTargets();
const Target* FindTarget(Machine machine);

// Digest the client checks: HMAC-SHA1(SEED64, auth73 || auth73).
std::vector<std::uint8_t> StockSeed64();

}  // namespace mangos::patcher
