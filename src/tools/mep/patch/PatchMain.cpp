#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "ClientFile.h"
#include "Hash.h"
#include "Patch.h"
#include "SecretFile.h"
#include "Targets.h"

namespace {

using namespace mangos::patcher;

const char* kUsage =
    "mep patch - patch a WoW 4.3.4.15595 client for MaNGOS's signed redirects\n"
    "\n"
    "  mep patch verify <exe>\n"
    "  mep patch apply  <exe> [options]\n"
    "  mep patch selftest\n"
    "\n"
    "apply options:\n"
    "  --secret <client.secret>    modulus and digest, as 'mep mint' wrote them\n"
    "  --modulus-le <512 hex>      take the modulus directly, little-endian\n"
    "  --auth73 <146 hex>          re-bake DIGEST20 for a custom auth blob\n"
    "  --digest <40 hex>           write DIGEST20 directly\n"
    "  --no-launcher               leave the manifest/Launcher check alone\n"
    "  --no-backup                 do not write <exe>.bak\n"
    "  --dry-run                   report what would change, write nothing\n"
    "  --allow-modified            patch an image that is neither stock nor already ours\n"
    "\n"
    "The three patches are modulus, DIGEST20 and the launcher bypass. This tool\n"
    "refuses to touch a client whose recv gate or send-slot routing has been\n"
    "rewritten: that client forces every opcode onto stream 0.\n";

std::string Hex64(std::uint64_t v) {
    std::ostringstream os;
    os << "0x" << std::uppercase << std::hex << v;
    return os.str();
}

void PrintSite(const SiteReport& s) {
    std::cout << "  " << std::left << std::setw(20) << s.name << std::setw(9)
              << SiteStateName(s.state) << "VA " << std::setw(12) << Hex64(s.va)
              << "file " << std::setw(10) << Hex64(s.file_offset);
    if (!s.detail.empty()) {
        std::cout << s.detail;
    }
    std::cout << "\n";
}

int PrintReport(const ClientFile& file, const ClientReport& report) {
    std::cout << "image      " << report.machine << ", base " << Hex64(report.image_base)
              << ", " << file.size() << " bytes\n";
    std::cout << "sha256     " << report.sha256 << "\n";

    if (report.target == nullptr) {
        std::cout << "target     UNKNOWN — no entry for this machine type\n";
        return 2;
    }
    std::cout << "target     " << report.target->label
              << (report.sha256_is_stock ? "  (stock image)" : "  (already modified)") << "\n";

    std::cout << "sites\n";
    PrintSite(report.modulus);
    PrintSite(report.digest);
    PrintSite(report.launcher);
    for (const SiteReport& s : report.forbidden) {
        PrintSite(s);
    }
    if (!report.target->forbidden_sites_known) {
        std::cout << "  (no verified MaNGOSPatcher sites for this machine; cannot be certified)\n";
    }

    if (report.HasForbiddenEdits()) {
        std::cout << "\nVERDICT    unusable — a MaNGOSPatcher-style edit collapses both\n"
                     "           streams onto slot 0. Restore a stock client.\n";
        return 3;
    }

    // All three sites, or the client passes every check here and is still refused
    // by the realm: a stock DIGEST20 rejects any auth blob but the stock one.
    const bool ready = report.target->forbidden_sites_known &&
                       report.modulus.state == SiteState::Applied &&
                       report.digest.state == SiteState::Applied &&
                       report.launcher.state == SiteState::Applied;
    std::cout << "\nVERDICT    " << (ready ? "patched for MaNGOS" : "not fully patched")
              << "\n";
    return ready ? 0 : 1;
}

int CommandVerify(const std::string& path) {
    ClientFile file;
    std::string error;
    if (!ClientFile::Load(path, file, error)) {
        std::cerr << "error: " << error << "\n";
        return 4;
    }
    return PrintReport(file, Inspect(file));
}

int CommandApply(const std::string& path, const std::vector<std::string>& args) {
    ApplyOptions options;
    std::string secret_path;
    std::string modulus_hex;
    std::string auth_hex;
    std::string digest_hex;
    bool modulus_is_big_endian = false;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto value = [&](std::string& dst) {
            if (i + 1 >= args.size()) {
                std::cerr << "error: " << a << " needs a value\n";
                return false;
            }
            dst = args[++i];
            return true;
        };
        if (a == "--secret") {
            if (!value(secret_path)) return 4;
        } else if (a == "--modulus-le") {
            if (!value(modulus_hex)) return 4;
        } else if (a == "--auth73") {
            if (!value(auth_hex)) return 4;
        } else if (a == "--digest") {
            if (!value(digest_hex)) return 4;
        } else if (a == "--no-launcher") {
            options.launcher = false;
        } else if (a == "--no-backup") {
            options.backup = false;
        } else if (a == "--dry-run") {
            options.dry_run = true;
        } else if (a == "--allow-modified") {
            options.allow_modified = true;
        } else {
            std::cerr << "error: unknown option " << a << "\n";
            return 4;
        }
    }

    if (!secret_path.empty() && !modulus_hex.empty()) {
        std::cerr << "error: give --secret or --modulus-le, not both\n";
        return 4;
    }
    if (!auth_hex.empty() && !digest_hex.empty()) {
        std::cerr << "error: give --auth73 or --digest, not both\n";
        return 4;
    }

    std::string error;
    if (!secret_path.empty()) {
        // One file carries both halves of what a client has to be told, so a
        // client patched from it cannot end up with a modulus from one
        // generation and a digest from another -- which produces a client that
        // passes every check here and is still refused by the server.
        if (!ReadSecretField(secret_path, "Modulus", modulus_hex, error)) {
            std::cerr << "error: " << error << "\n";
            return 4;
        }
        if (digest_hex.empty() && auth_hex.empty() &&
            !ReadSecretField(secret_path, "Digest", digest_hex, error)) {
            std::cerr << "error: " << error << "\n";
            return 4;
        }
        // Both secret files state the modulus the way the server does, as a
        // big-endian number. The client stores it the other way round.
        modulus_is_big_endian = true;
    }
    if (!modulus_hex.empty() && !FromHex(modulus_hex, options.modulus_le)) {
        std::cerr << "error: modulus is not hex\n";
        return 4;
    }
    if (modulus_is_big_endian) {
        std::reverse(options.modulus_le.begin(), options.modulus_le.end());
    }
    if (!digest_hex.empty() && !FromHex(digest_hex, options.digest20)) {
        std::cerr << "error: digest is not hex\n";
        return 4;
    }
    if (!auth_hex.empty()) {
        std::vector<std::uint8_t> auth;
        if (!FromHex(auth_hex, auth)) {
            std::cerr << "error: auth blob is not hex\n";
            return 4;
        }
        if (!DigestForAuthBlob(auth, options.digest20, error)) {
            std::cerr << "error: " << error << "\n";
            return 4;
        }
        std::cout << "DIGEST20 for this auth blob: " << ToHex(options.digest20.data(), 20)
                  << "\n";
    }

    if (options.modulus_le.empty() && options.digest20.empty() && !options.launcher) {
        std::cerr << "error: nothing to do\n";
        return 4;
    }

    ClientFile file;
    if (!ClientFile::Load(path, file, error)) {
        std::cerr << "error: " << error << "\n";
        return 4;
    }

    const ClientReport report = Inspect(file);
    PrintReport(file, report);
    std::cout << "\n";

    const ApplyResult result = Apply(file, report, options);
    for (const std::string& action : result.actions) {
        std::cout << "  - " << action << "\n";
    }
    if (!result.ok) {
        std::cerr << "error: " << result.error << "\n";
        return 5;
    }
    if (!result.changed) {
        std::cout << "nothing to write; client already in the requested state\n";
        return 0;
    }
    if (options.dry_run) {
        std::cout << "dry run — no bytes written\n";
        return 0;
    }

    if (!file.Save(path, options.backup, error)) {
        std::cerr << "error: " << error << "\n";
        return 5;
    }
    std::cout << "wrote " << path << " (" << file.size() << " bytes, size unchanged)\n";
    if (options.backup) {
        std::cout << "backup " << path << ".bak\n";
    }

    ClientFile after;
    if (!ClientFile::Load(path, after, error)) {
        std::cerr << "error: cannot re-read for verification: " << error << "\n";
        return 5;
    }
    std::cout << "\nre-read:\n";
    return PrintReport(after, Inspect(after)) == 0 ? 0 : 5;
}

bool Expect(const std::string& what, const std::string& got, const std::string& want) {
    if (got == want) {
        std::cout << "  ok   " << what << "\n";
        return true;
    }
    std::cout << "  FAIL " << what << "\n       got  " << got << "\n       want " << want
              << "\n";
    return false;
}

int CommandSelfTest() {
    bool ok = true;
    const std::string abc = "abc";
    const auto* p = reinterpret_cast<const std::uint8_t*>(abc.data());

    ok &= Expect("SHA-256(\"abc\")", ToHex(Sha256(p, abc.size())),
                 "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD");
    ok &= Expect("SHA-1(\"abc\")", ToHex(Sha1(p, abc.size())),
                 "A9993E364706816ABA3E25717850C26C9CD0D89D");

    // RFC 2202 test case 2.
    const std::string key = "Jefe";
    const std::string msg = "what do ya want for nothing?";
    ok &= Expect("HMAC-SHA1(RFC 2202 #2)",
                 ToHex(HmacSha1(std::vector<std::uint8_t>(key.begin(), key.end()),
                                std::vector<std::uint8_t>(msg.begin(), msg.end()))),
                 "EFFCDF6AE5EB2FA2D27416D5F184DF9C259A7C79");

    for (const Target& t : KnownTargets()) {
        const bool sizes = t.modulus.stock.size() == 256u && t.seed.stock.size() == 64u &&
                           t.digest.stock.size() == 20u && !t.launcher.expect.empty() &&
                           t.launcher.patch.size() == 5u;
        ok &= Expect(t.label + " table sizes", sizes ? "ok" : "bad", "ok");
        ok &= Expect(t.label + " sha256 length",
                     std::to_string(t.stock_sha256.size()), "64");
    }

    std::cout << (ok ? "selftest OK\n" : "selftest FAILED\n");
    return ok ? 0 : 1;
}

}  // namespace

int PatchMain(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty() || args[0] == "-h" || args[0] == "--help") {
        std::cout << kUsage;
        return args.empty() ? 4 : 0;
    }

    const std::string& command = args[0];
    if (command == "selftest") {
        return CommandSelfTest();
    }
    if (args.size() < 2) {
        std::cerr << "error: " << command << " needs a client path\n";
        return 4;
    }
    if (command == "verify") {
        return CommandVerify(args[1]);
    }
    if (command == "apply") {
        return CommandApply(args[1], std::vector<std::string>(args.begin() + 2, args.end()));
    }

    std::cerr << "error: unknown command " << command << "\n\n" << kUsage;
    return 4;
}
