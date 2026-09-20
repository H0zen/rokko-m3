#include "PeImage.h"

#include <cstring>

namespace mangos::patcher {
namespace {

constexpr std::uint16_t kDosMagic = 0x5A4D;      // "MZ"
constexpr std::uint32_t kPeSignature = 0x00004550;  // "PE\0\0"
constexpr std::uint16_t kOptionalPe32 = 0x010B;
constexpr std::uint16_t kOptionalPe32Plus = 0x020B;
constexpr std::size_t kSectionHeaderSize = 40;

bool Read16(const std::vector<std::uint8_t>& f, std::size_t off, std::uint16_t& out) {
    if (off > f.size() || f.size() - off < 2) {
        return false;
    }
    out = static_cast<std::uint16_t>(f[off]) |
          static_cast<std::uint16_t>(static_cast<std::uint16_t>(f[off + 1]) << 8);
    return true;
}

bool Read32(const std::vector<std::uint8_t>& f, std::size_t off, std::uint32_t& out) {
    if (off > f.size() || f.size() - off < 4) {
        return false;
    }
    out = static_cast<std::uint32_t>(f[off]) |
          (static_cast<std::uint32_t>(f[off + 1]) << 8) |
          (static_cast<std::uint32_t>(f[off + 2]) << 16) |
          (static_cast<std::uint32_t>(f[off + 3]) << 24);
    return true;
}

bool Read64(const std::vector<std::uint8_t>& f, std::size_t off, std::uint64_t& out) {
    std::uint32_t lo = 0;
    std::uint32_t hi = 0;
    if (!Read32(f, off, lo) || !Read32(f, off + 4, hi)) {
        return false;
    }
    out = static_cast<std::uint64_t>(lo) | (static_cast<std::uint64_t>(hi) << 32);
    return true;
}

}  // namespace

std::optional<PeImage> PeImage::Parse(const std::vector<std::uint8_t>& file,
                                      std::string& error) {
    std::uint16_t dos = 0;
    if (!Read16(file, 0, dos) || dos != kDosMagic) {
        error = "not a PE image (no MZ header)";
        return std::nullopt;
    }

    std::uint32_t lfanew = 0;
    if (!Read32(file, 0x3C, lfanew)) {
        error = "truncated DOS header";
        return std::nullopt;
    }

    std::uint32_t sig = 0;
    if (!Read32(file, lfanew, sig) || sig != kPeSignature) {
        error = "not a PE image (no PE signature)";
        return std::nullopt;
    }

    const std::size_t coff = static_cast<std::size_t>(lfanew) + 4u;
    std::uint16_t machine = 0;
    std::uint16_t section_count = 0;
    std::uint16_t optional_size = 0;
    if (!Read16(file, coff + 0, machine) || !Read16(file, coff + 2, section_count) ||
        !Read16(file, coff + 16, optional_size)) {
        error = "truncated COFF header";
        return std::nullopt;
    }

    const std::size_t optional = coff + 20u;
    std::uint16_t optional_magic = 0;
    if (!Read16(file, optional, optional_magic)) {
        error = "truncated optional header";
        return std::nullopt;
    }

    PeImage image;
    image.machine_ = static_cast<Machine>(machine);

    if (optional_magic == kOptionalPe32) {
        std::uint32_t base32 = 0;
        if (!Read32(file, optional + 28, base32)) {
            error = "truncated PE32 optional header";
            return std::nullopt;
        }
        image.image_base_ = base32;
    } else if (optional_magic == kOptionalPe32Plus) {
        if (!Read64(file, optional + 24, image.image_base_)) {
            error = "truncated PE32+ optional header";
            return std::nullopt;
        }
    } else {
        error = "unknown optional header magic";
        return std::nullopt;
    }

    const std::uint64_t table = static_cast<std::uint64_t>(optional) + optional_size;
    for (std::uint16_t i = 0; i < section_count; ++i) {
        const std::uint64_t off64 = table + static_cast<std::uint64_t>(i) * kSectionHeaderSize;
        if (off64 > file.size() || file.size() - off64 < kSectionHeaderSize) {
            error = "truncated section table";
            return std::nullopt;
        }
        const std::size_t off = static_cast<std::size_t>(off64);
        Section s;
        const char* raw_name = reinterpret_cast<const char*>(file.data() + off);
        s.name.assign(raw_name, ::strnlen(raw_name, 8));
        if (!Read32(file, off + 8, s.virtual_size) ||
            !Read32(file, off + 12, s.virtual_address) ||
            !Read32(file, off + 16, s.raw_size) ||
            !Read32(file, off + 20, s.raw_pointer)) {
            error = "truncated section header";
            return std::nullopt;
        }
        image.sections_.push_back(std::move(s));
    }

    if (image.sections_.empty()) {
        error = "no sections";
        return std::nullopt;
    }
    return image;
}

std::optional<std::uint64_t> PeImage::VirtualAddressOf(std::size_t file_offset) const {
    for (const Section& s : sections_) {
        if (s.raw_size == 0) {
            continue;
        }
        if (file_offset >= s.raw_pointer &&
            file_offset < static_cast<std::uint64_t>(s.raw_pointer) + s.raw_size) {
            const std::uint64_t delta = file_offset - s.raw_pointer;
            return image_base_ + s.virtual_address + delta;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> PeImage::FileOffsetOf(std::uint64_t va) const {
    if (va < image_base_) {
        return std::nullopt;
    }
    const std::uint64_t rva = va - image_base_;
    for (const Section& s : sections_) {
        if (rva >= s.virtual_address &&
            rva < static_cast<std::uint64_t>(s.virtual_address) + s.virtual_size) {
            const std::uint64_t delta = rva - s.virtual_address;
            if (delta >= s.raw_size) {
                return std::nullopt;  // in the zero-fill tail, not in the file
            }
            return static_cast<std::size_t>(static_cast<std::uint64_t>(s.raw_pointer) + delta);
        }
    }
    return std::nullopt;
}

std::string PeImage::MachineName() const {
    switch (machine_) {
        case Machine::I386:
            return "x86";
        case Machine::Amd64:
            return "x64";
        default:
            return "unknown";
    }
}

}  // namespace mangos::patcher
