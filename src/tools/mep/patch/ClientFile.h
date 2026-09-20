#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Hash.h"
#include "PeImage.h"

namespace mangos::patcher {

// The client image in memory, plus the PE map that turns the disassembly's
// virtual addresses into offsets in this buffer.
class ClientFile {
public:
    static bool Load(const std::string& path, ClientFile& out, std::string& error);

    bool Save(const std::string& path, bool backup, std::string& error) const;

    const PeImage& pe() const { return pe_; }
    const std::vector<std::uint8_t>& bytes() const { return bytes_; }
    std::size_t size() const { return bytes_.size(); }

    Sha256Digest Digest() const { return Sha256(bytes_); }

    // All occurrences of `pattern`, as file offsets.
    std::vector<std::size_t> FindAll(const std::vector<std::uint8_t>& pattern) const;

    bool ReadAtVa(std::uint64_t va, std::size_t len, std::vector<std::uint8_t>& out) const;
    bool WriteAtVa(std::uint64_t va, const std::vector<std::uint8_t>& data, std::string& error);
    bool WriteAtOffset(std::size_t offset, const std::vector<std::uint8_t>& data);

private:
    std::vector<std::uint8_t> bytes_;
    PeImage pe_;
};

}  // namespace mangos::patcher
