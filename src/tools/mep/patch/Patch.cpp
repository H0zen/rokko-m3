#include "Patch.h"

#include <algorithm>

namespace mangos::patcher {
namespace {

// A data blob is located by content and then confirmed against the address the
// disassembly cites. When the stock content is gone the site is found by VA
// instead, so a re-run can still report what is there.
SiteReport InspectDataSite(const ClientFile& file, const DataSite& site) {
    SiteReport r;
    r.name = site.name;
    r.va = site.expect_va;

    const std::vector<std::size_t> hits = file.FindAll(site.stock);
    if (hits.size() == 1) {
        const std::optional<std::uint64_t> va = file.pe().VirtualAddressOf(hits.front());
        r.state = SiteState::Stock;
        r.file_offset = hits.front();
        if (!va.has_value()) {
            r.detail = "stock content, but the offset is outside every section";
        } else if (*va != site.expect_va) {
            r.detail = "stock content at an unexpected address";
            r.va = *va;
        } else {
            r.va = *va;
        }
        return r;
    }
    if (hits.size() > 1) {
        r.state = SiteState::Foreign;
        r.detail = "stock content found at " + std::to_string(hits.size()) + " addresses";
        return r;
    }

    const std::optional<std::size_t> off = file.pe().FileOffsetOf(site.expect_va);
    if (!off.has_value()) {
        r.state = SiteState::Missing;
        r.detail = "address not backed by file bytes";
        return r;
    }
    std::vector<std::uint8_t> now;
    if (!file.ReadAtVa(site.expect_va, site.stock.size(), now)) {
        r.state = SiteState::Missing;
        return r;
    }
    r.state = SiteState::Applied;
    r.file_offset = *off;
    r.detail = ToHex(now.data(), std::min<std::size_t>(now.size(), 8u)) + "...";
    return r;
}

SiteReport InspectCodeSite(const ClientFile& file, const CodeSite& site) {
    SiteReport r;
    r.name = site.name;
    r.va = site.va;

    std::optional<std::size_t> off;
    if (site.by_file_offset) {
        off = site.file_offset;
        const std::optional<std::uint64_t> va = file.pe().VirtualAddressOf(site.file_offset);
        r.va = va.value_or(0);
    } else {
        off = file.pe().FileOffsetOf(site.va);
    }
    if (!off.has_value()) {
        r.state = SiteState::Missing;
        r.detail = "address not backed by file bytes";
        return r;
    }
    r.file_offset = *off;

    std::vector<std::uint8_t> now;
    if (*off > file.size() || file.size() - *off < site.expect.size()) {
        r.state = SiteState::Missing;
        r.detail = "offset not backed by file bytes";
        return r;
    }
    now.assign(file.bytes().begin() + static_cast<std::ptrdiff_t>(*off),
               file.bytes().begin() + static_cast<std::ptrdiff_t>(*off + site.expect.size()));

    if (now == site.expect) {
        r.state = SiteState::Stock;
        r.detail = ToHex(now.data(), now.size());
        return r;
    }

    if (!site.patch.empty() && now.size() >= site.patch.size()) {
        const bool head_matches =
            std::equal(site.patch.begin(), site.patch.end(), now.begin());
        const bool tail_matches =
            std::equal(site.expect.begin() + static_cast<std::ptrdiff_t>(site.patch.size()),
                       site.expect.end(),
                       now.begin() + static_cast<std::ptrdiff_t>(site.patch.size()));
        if (head_matches && tail_matches) {
            r.state = SiteState::Applied;
            r.detail = ToHex(now.data(), now.size());
            return r;
        }
    }

    r.state = SiteState::Foreign;
    r.detail = ToHex(now.data(), now.size());
    return r;
}

}  // namespace

bool ClientReport::HasForbiddenEdits() const {
    for (const SiteReport& s : forbidden) {
        if (s.state != SiteState::Stock) {
            return true;
        }
    }
    return false;
}

const char* SiteStateName(SiteState state) {
    switch (state) {
        case SiteState::Stock:
            return "stock";
        case SiteState::Applied:
            return "patched";
        case SiteState::Foreign:
            return "FOREIGN";
        case SiteState::Missing:
            return "MISSING";
    }
    return "?";
}

ClientReport Inspect(const ClientFile& file) {
    ClientReport report;
    report.machine = file.pe().MachineName();
    report.image_base = file.pe().image_base();
    report.sha256 = ToHex(file.Digest());
    report.target = FindTarget(file.pe().machine());
    if (report.target == nullptr) {
        return report;
    }

    report.sha256_is_stock = (report.sha256 == report.target->stock_sha256);
    report.modulus = InspectDataSite(file, report.target->modulus);
    report.digest = InspectDataSite(file, report.target->digest);
    report.launcher = InspectCodeSite(file, report.target->launcher);
    for (const CodeSite& site : report.target->forbidden) {
        report.forbidden.push_back(InspectCodeSite(file, site));
    }
    return report;
}

bool DigestForAuthBlob(const std::vector<std::uint8_t>& auth73,
                       std::vector<std::uint8_t>& out, std::string& error) {
    if (auth73.size() != 73u) {
        error = "auth blob must be 73 bytes, got " + std::to_string(auth73.size());
        return false;
    }
    std::vector<std::uint8_t> msg;
    msg.reserve(auth73.size() * 2u);
    msg.insert(msg.end(), auth73.begin(), auth73.end());
    msg.insert(msg.end(), auth73.begin(), auth73.end());

    const Sha1Digest d = HmacSha1(StockSeed64(), msg);
    out.assign(d.begin(), d.end());
    return true;
}

ApplyResult Apply(ClientFile& file, const ClientReport& report,
                  const ApplyOptions& options) {
    ApplyResult result;

    if (report.target == nullptr) {
        result.error = "unknown client: no target for machine " + report.machine;
        return result;
    }
    if (!report.target->forbidden_sites_known) {
        result.error =
            "refusing: the MaNGOSPatcher edit sites are not verified for " +
            report.target->label +
            " yet, so a collapsed-stream client cannot be told from a stock one. "
            "Patch the 32-bit client.";
        return result;
    }
    if (report.HasForbiddenEdits()) {
        result.error =
            "refusing: a MaNGOSPatcher-style edit is present. That client forces "
            "every opcode onto stream 0 and cannot run the dual-stream protocol.";
        return result;
    }
    // Only an image whose layout is known is written into: the stock client (by
    // hash), or one this tool already patched (its own launcher bytes are there).
    // Anything else is some other build or somebody else's edit.
    if (!report.sha256_is_stock && report.launcher.state != SiteState::Applied &&
        !options.allow_modified) {
        result.error =
            "refusing: this is not the stock " + report.target->label +
            " (SHA-256 differs) and it does not carry this tool's launcher patch, so "
            "its layout is unknown. Pass --allow-modified to patch it anyway.";
        return result;
    }

    if (!options.modulus_le.empty()) {
        if (options.modulus_le.size() != 256u) {
            result.error = "modulus must be exactly 256 bytes, got " +
                           std::to_string(options.modulus_le.size());
            return result;
        }
        if (report.modulus.state == SiteState::Missing ||
            report.modulus.state == SiteState::Foreign) {
            result.error = "cannot locate the redirect modulus (" + report.modulus.detail + ")";
            return result;
        }

        std::vector<std::uint8_t> now;
        file.ReadAtVa(report.modulus.va, 256u, now);
        if (now == options.modulus_le) {
            result.actions.push_back("modulus already current");
        } else {
            if (!options.dry_run) {
                if (!file.WriteAtOffset(report.modulus.file_offset, options.modulus_le)) {
                    result.error = "write outside the image";
                    return result;
                }
            }
            result.changed = true;
            result.actions.push_back("modulus written (256 B)");
        }
    }

    if (!options.digest20.empty()) {
        if (options.digest20.size() != 20u) {
            result.error = "digest must be exactly 20 bytes";
            return result;
        }
        if (report.digest.state == SiteState::Missing ||
            report.digest.state == SiteState::Foreign) {
            result.error = "cannot locate DIGEST20 (" + report.digest.detail + ")";
            return result;
        }

        std::vector<std::uint8_t> now;
        file.ReadAtVa(report.digest.va, 20u, now);
        if (now == options.digest20) {
            result.actions.push_back("DIGEST20 already current");
        } else {
            if (!options.dry_run) {
                if (!file.WriteAtOffset(report.digest.file_offset, options.digest20)) {
                    result.error = "write outside the image";
                    return result;
                }
            }
            result.changed = true;
            result.actions.push_back("DIGEST20 written (20 B)");
        }
    }

    if (options.launcher) {
        switch (report.launcher.state) {
            case SiteState::Applied:
                result.actions.push_back("launcher bypass already present");
                break;
            case SiteState::Stock: {
                if (!options.dry_run) {
                    if (!file.WriteAtOffset(report.launcher.file_offset,
                                       report.target->launcher.patch)) {
                        result.error = "write outside the image";
                        return result;
                    }
                }
                result.changed = true;
                result.actions.push_back("launcher bypass written (mov eax,1)");
                break;
            }
            case SiteState::Foreign:
                result.error = "manifest-check site holds unexpected bytes (" +
                               report.launcher.detail + "); refusing to patch it";
                return result;
            case SiteState::Missing:
                result.error = "manifest-check site is not in this image";
                return result;
        }
    }

    result.ok = true;
    return result;
}

}  // namespace mangos::patcher
