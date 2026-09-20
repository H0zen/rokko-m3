#include "Targets.h"

#include <stdexcept>

#include "Hash.h"

namespace mangos::patcher {
namespace {

CodeSite OffsetSite(const char* name, std::size_t file_offset,
                    std::vector<std::uint8_t> stock, std::vector<std::uint8_t> rewritten) {
    CodeSite s;
    s.name = name;
    s.expect = std::move(stock);
    s.patch = std::move(rewritten);
    s.by_file_offset = true;
    s.file_offset = file_offset;
    return s;
}

std::vector<std::uint8_t> Hex(const std::string& text) {
    std::vector<std::uint8_t> out;
    if (!FromHex(text, out)) {
        throw std::logic_error("bad hex literal in the target table");
    }
    return out;
}

// Wow.exe 0x00B987A0 / Wow-64.exe 0x140978500, little-endian as stored.
const char* kStockModulusLe =
    "91D59BB7D4E183A5222B5F38F4B886FF3284382D99388FBAF3C9225D51731E28"
    "87248FB5C9B07C95D06B5BF494C5949DFA6F473AA386C0A837F39BEF2FC1FBB3"
    "F41C2B0ED36D88BB02E04E63FA76E343F901FD235E6A0B14EC5E91340D0B4FA3"
    "5A46C55EDCB5CDC1476B59CAFAA9BE249FF5056BBB678BB7E43A43005C1CB7CA"
    "989079776D054F83CCAC062E50118723D8A6F76F7A5987A6DE5DD8EC44BE4531"
    "7F8AF058895374DFCCAD0124D819651C25D3E16B8BDAFE1DA42C8B25ED7CFF6A"
    "E063D052207E6249D2B36BCC9169A5088B6965FFB9C917025DD88E1A63D92A7F"
    "DBE3F8766DEA0E36987819C587BA6C20B60844044CA8D5B69D4D002040009004";

// Wow.exe 0x00B98740 / Wow-64.exe 0x1409784B0.
const char* kSeed64 =
    "2C1F1D80C38C2364DA90CA8E2CFC0CCE09D362F9F38BBE9F19EF58A11C341441"
    "3F23FDD3E814EC2AFD4F95BA307E565D83958169B05AB49DA855FFFCEE580A2F";

// Wow.exe 0x00B98780 / Wow-64.exe 0x140978498.
const char* kDigest20 = "B30774E3BB51FB2B511487241D4CB82B0EC2631F";

// mov eax, 1 — makes the manifest check's `test eax,eax / jnz` always continue.
const char* kMovEax1 = "B801000000";

std::vector<Target> BuildTargets() {
    std::vector<Target> targets;

    {
        Target t;
        t.label = "Wow.exe 4.3.4.15595";
        t.machine = Machine::I386;
        t.stock_sha256 =
            "DE6F3B0F0C974CB2B9EB713A6A14573F995D0AD99841221A3A535B3719767BD4";

        t.modulus = {"redirect modulus", 0x00B987A0u, Hex(kStockModulusLe)};
        t.seed = {"SEED64", 0x00B98740u, Hex(kSeed64)};
        t.digest = {"DIGEST20", 0x00B98780u, Hex(kDigest20)};

        // .text:00407F7A call sub_406D30 / test eax,eax / jnz short loc_407FD5
        t.launcher = {"manifest check", 0x00407F7Au, Hex("E8B1EDFFFF85C075"),
                      Hex(kMovEax1)};

        // .text:00488FAE jz short loc_488FD3 — the recv gate on sub_487AB0.
        t.forbidden.push_back({"recv gate (site B)", 0x00488FAEu, Hex("74"), {}});
        // .text:004895CA mov edx,[ebp+arg_4] — send-slot selection in sub_489590.
        t.forbidden.push_back({"send slot routing", 0x004895CAu, Hex("8B550C"), {}});
        t.forbidden_sites_known = true;
        targets.push_back(std::move(t));
    }

    {
        Target t;
        t.label = "Wow-64.exe 4.3.4.15595";
        t.machine = Machine::Amd64;
        t.stock_sha256 =
            "4D2B2CA627A1117C8DB13C4D3689C03C9FB4D7BB3695C99789F9F61C5D4899CE";

        t.modulus = {"redirect modulus", 0x140978500ull, Hex(kStockModulusLe)};
        t.seed = {"SEED64", 0x1409784B0ull, Hex(kSeed64)};
        t.digest = {"DIGEST20", 0x140978498ull, Hex(kDigest20)};

        // .text:14000A0E5 call sub_140008610 / test eax,eax / jnz short loc_14000A152.
        // The byte run is not unique in this image, so the site is addressed by
        // VA and only then checked against `expect`.
        t.launcher = {"manifest check", 0x14000A0E5ull, Hex("E826E5FFFF85C075"),
                      Hex(kMovEax1)};
        // The MaNGOSPatcher edits for the 64-bit client, as that tool's own table
        // records them (mangosthree/tools, MaNGOSPatcher/Patcher/Patches.cs,
        // Patch434Win64: file offsets, stock bytes, rewritten bytes). Its x86
        // table maps onto the three x86 sites above at a constant delta, which
        // is the evidence the two tables describe the same edits. Any of these
        // rewritten collapses both streams onto slot 0.
        t.forbidden.push_back(OffsetSite("send: je skipping the type-0 path", 0xAAB6Fu,
                                         Hex("7408"), Hex("9090")));
        t.forbidden.push_back(OffsetSite("send: forced type 0", 0xAAB71u,
                                         Hex("418BD5"), Hex("31D290")));
        t.forbidden.push_back(OffsetSite("NetClient: forced connection[0]", 0xA9AD3u,
                                         Hex("7410"), Hex("EB10")));
        t.forbidden_sites_known = true;
        targets.push_back(std::move(t));
    }

    return targets;
}

}  // namespace

const std::vector<Target>& KnownTargets() {
    static const std::vector<Target> kTargets = BuildTargets();
    return kTargets;
}

const Target* FindTarget(Machine machine) {
    for (const Target& t : KnownTargets()) {
        if (t.machine == machine) {
            return &t;
        }
    }
    return nullptr;
}

std::vector<std::uint8_t> StockSeed64() {
    return Hex(kSeed64);
}

}  // namespace mangos::patcher
