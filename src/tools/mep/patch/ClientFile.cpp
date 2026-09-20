#include "ClientFile.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace mangos::patcher {

bool ClientFile::Load(const std::string& path, ClientFile& out, std::string& error) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        error = "cannot open " + path;
        return false;
    }
    const std::streamsize len = in.tellg();
    if (len <= 0) {
        error = "empty file: " + path;
        return false;
    }
    in.seekg(0, std::ios::beg);

    out.bytes_.resize(static_cast<std::size_t>(len));
    if (!in.read(reinterpret_cast<char*>(out.bytes_.data()), len)) {
        error = "short read on " + path;
        return false;
    }

    std::string pe_error;
    const std::optional<PeImage> pe = PeImage::Parse(out.bytes_, pe_error);
    if (!pe.has_value()) {
        error = path + ": " + pe_error;
        return false;
    }
    out.pe_ = *pe;
    return true;
}

bool ClientFile::Save(const std::string& path, bool backup, std::string& error) const {
    namespace fs = std::filesystem;
    std::error_code ec;
    // The backup is the stock client, taken once. A later run must not replace it
    // with an already patched copy, so an existing one is kept exactly as it is.
    if (backup) {
        const fs::path dst(path + ".bak");
        if (!fs::exists(dst, ec)) {
            fs::copy_file(fs::path(path), dst, ec);
            if (ec) {
                error = "cannot write backup " + dst.string() + ": " + ec.message();
                return false;
            }
        }
    }
    // Never rewrite the executable in place: a short write, a full disk or a crash
    // would leave the player a truncated client. Write beside it, verify the size,
    // then swap the two names in one step.
    const std::string tmp = path + ".mep.tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            error = "cannot create " + tmp;
            return false;
        }
        f.write(reinterpret_cast<const char*>(bytes_.data()),
                static_cast<std::streamsize>(bytes_.size()));
        f.flush();
        if (!f) {
            error = "write failed: " + tmp;
            fs::remove(tmp, ec);
            return false;
        }
    }
    const std::uintmax_t written = fs::file_size(tmp, ec);
    if (ec || written != bytes_.size()) {
        error = "short write: " + tmp;
        fs::remove(tmp, ec);
        return false;
    }
    // The new file must be the executable the old one was: carry its permission
    // bits over before it takes the old name (on POSIX a fresh file is not 0755).
    {
        std::error_code pec;
        const fs::file_status original = fs::status(path, pec);
        if (!pec) {
            fs::permissions(tmp, original.permissions(), pec);
        }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        error = "cannot replace " + path + ": " + ec.message();
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

std::vector<std::size_t> ClientFile::FindAll(const std::vector<std::uint8_t>& pattern) const {
    std::vector<std::size_t> hits;
    if (pattern.empty() || pattern.size() > bytes_.size()) {
        return hits;
    }
    auto it = bytes_.begin();
    while (true) {
        it = std::search(it, bytes_.end(), pattern.begin(), pattern.end());
        if (it == bytes_.end()) {
            return hits;
        }
        hits.push_back(static_cast<std::size_t>(it - bytes_.begin()));
        ++it;
    }
}

bool ClientFile::ReadAtVa(std::uint64_t va, std::size_t len,
                          std::vector<std::uint8_t>& out) const {
    const std::optional<std::size_t> off = pe_.FileOffsetOf(va);
    if (!off.has_value() || *off > bytes_.size() || bytes_.size() - *off < len) {
        return false;
    }
    out.assign(bytes_.begin() + static_cast<std::ptrdiff_t>(*off),
               bytes_.begin() + static_cast<std::ptrdiff_t>(*off + len));
    return true;
}

bool ClientFile::WriteAtVa(std::uint64_t va, const std::vector<std::uint8_t>& data,
                           std::string& error) {
    const std::optional<std::size_t> off = pe_.FileOffsetOf(va);
    if (!off.has_value() || !WriteAtOffset(*off, data)) {
        error = "virtual address is not backed by file bytes";
        return false;
    }
    return true;
}

bool ClientFile::WriteAtOffset(std::size_t offset, const std::vector<std::uint8_t>& data) {
    if (offset > bytes_.size() || bytes_.size() - offset < data.size()) {
        return false;
    }
    std::copy(data.begin(), data.end(),
              bytes_.begin() + static_cast<std::ptrdiff_t>(offset));
    return true;
}

}  // namespace mangos::patcher
