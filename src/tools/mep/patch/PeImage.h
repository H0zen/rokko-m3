#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mangos::patcher {

enum class Machine : std::uint16_t {
    Unknown = 0x0000,
    I386 = 0x014C,
    Amd64 = 0x8664,
};

struct Section {
    std::string name;
    std::uint32_t virtual_address = 0;
    std::uint32_t virtual_size = 0;
    std::uint32_t raw_pointer = 0;
    std::uint32_t raw_size = 0;
};

// Just enough PE to turn a file offset into the virtual address the
// disassembly cites, and back. Nothing is mapped or executed.
class PeImage {
public:
    static std::optional<PeImage> Parse(const std::vector<std::uint8_t>& file,
                                        std::string& error);

    Machine machine() const { return machine_; }
    std::uint64_t image_base() const { return image_base_; }
    const std::vector<Section>& sections() const { return sections_; }

    std::optional<std::uint64_t> VirtualAddressOf(std::size_t file_offset) const;
    std::optional<std::size_t> FileOffsetOf(std::uint64_t va) const;

    std::string MachineName() const;

private:
    Machine machine_ = Machine::Unknown;
    std::uint64_t image_base_ = 0;
    std::vector<Section> sections_;
};

}  // namespace mangos::patcher
