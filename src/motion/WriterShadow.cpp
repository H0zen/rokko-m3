/**
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * MaNGOS is a full featured server for World of Warcraft, supporting
 * the following clients: 1.12.x, 2.4.3, 3.3.5a, 4.3.4a and 5.4.8
 *
 * Copyright (C) 2005-2026 MaNGOS <https://www.getmangos.eu>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * World of Warcraft, and all World of Warcraft or Warcraft art, images,
 * and lore are copyrighted by Blizzard Entertainment, Inc.
 */

#include "WriterShadow.h"
#include "Writers.h"

#include <cstdio>

namespace Motion
{
    ShadowResult JudgeWriter(uint64 guid, Change const& change, Mode mode, uint32 legacyCounter, WorldPacket const& legacy)
    {
        ShadowResult r;
        r.verdict = ShadowVerdict::NoWriter;
        r.legacyOpcode = legacy.GetOpcode();
        r.kernelOpcode = 0;
        r.firstDifference = size_t(-1);
        r.legacySize = legacy.size();
        r.kernelSize = 0;

        bool legacyIsSpline = false;
        MatrixRow const* row = RowForOpcode(r.legacyOpcode, &legacyIsSpline);
        if (!row || row->type != change.type) { return r; }

        WorldPacket kernel;
        if (legacyIsSpline)
        {
            if (mode == Mode::ClientDriven) { r.verdict = ShadowVerdict::LegacyOnly; return r; }
            if (!BuildSpline(kernel, guid, change)) { return r; }
        }
        else
        {
            if (mode == Mode::ServerDriven) { return r; }
            if (!BuildMover(kernel, guid, legacyCounter, change)) { return r; }
        }
        r.kernelOpcode = kernel.GetOpcode();
        r.kernelSize = kernel.size();

        if (r.kernelOpcode == r.legacyOpcode)
        {
            const size_t n = r.kernelSize < r.legacySize ? r.kernelSize : r.legacySize;
            size_t first = size_t(-1);
            for (size_t i = 0; i < n; ++i)
            {
                if (kernel.contents()[i] != legacy.contents()[i]) { first = i; break; }
            }
            if (first == size_t(-1) && r.kernelSize != r.legacySize) { first = n; }
            if (first == size_t(-1)) { r.verdict = ShadowVerdict::Exact; return r; }
            r.firstDifference = first;
        }
        const bool known = change.type == ChangeType::CollisionHeight
            || (change.type == ChangeType::GravityDisabled && mode == Mode::ClientDriven);
        r.verdict = known ? ShadowVerdict::KnownDifferent : ShadowVerdict::Mismatch;
        return r;
    }

    ShadowCounters::Row::Row()
        : seen(0), exact(0), mismatch(0), knownDifferent(0), legacyOnly(0), noWriter(0), firstDifference(0), otherOpcode(0)
    {
    }

    ShadowCounters::ShadowCounters() : m_seen(0), m_mismatched(0)
    {
    }

    void ShadowCounters::Count(ShadowResult const& result)
    {
        bool isSpline = false;
        MatrixRow const* row = RowForOpcode(result.legacyOpcode, &isSpline);
        size_t index = kRows - 1;
        if (row)
        {
            for (size_t i = 0; i < MatrixSize(); ++i)
            {
                if (&MatrixRowAt(i) == row) { index = i * kForms + (isSpline ? 1 : 0); break; }
            }
        }
        // A matrix grown past what kRows was sized for lands here instead of past m_rows.
        if (index >= kRows - 1) { index = kRows - 1; }
        Row& r = m_rows[index];
        r.seen.fetch_add(1, std::memory_order_relaxed);
        m_seen.fetch_add(1, std::memory_order_relaxed);
        switch (result.verdict)
        {
            case ShadowVerdict::Exact:          r.exact.fetch_add(1, std::memory_order_relaxed); break;
            case ShadowVerdict::KnownDifferent: r.knownDifferent.fetch_add(1, std::memory_order_relaxed); break;
            case ShadowVerdict::LegacyOnly:     r.legacyOnly.fetch_add(1, std::memory_order_relaxed); break;
            case ShadowVerdict::NoWriter:
                r.noWriter.fetch_add(1, std::memory_order_relaxed);
                if (!row) { r.otherOpcode.store(result.legacyOpcode, std::memory_order_relaxed); }
                break;
            case ShadowVerdict::Mismatch:
                r.mismatch.fetch_add(1, std::memory_order_relaxed);
                m_mismatched.fetch_add(1, std::memory_order_relaxed);
                if (r.firstDifference.load(std::memory_order_relaxed) == 0)
                {
                    r.firstDifference.store(result.firstDifference == size_t(-1) ? 0xFFFFFFFFu : uint32(result.firstDifference) + 1, std::memory_order_relaxed);
                    r.otherOpcode.store(result.kernelOpcode, std::memory_order_relaxed);
                }
                break;
        }
    }

    void ShadowCounters::Report(std::function<void(std::string const&)> const& line) const
    {
        uint32 seen = 0, exact = 0, mismatch = 0, known = 0, legacyOnly = 0, noWriter = 0;
        for (size_t i = 0; i < kRows; ++i)
        {
            Row const& r = m_rows[i];
            seen += r.seen.load(std::memory_order_relaxed);
            exact += r.exact.load(std::memory_order_relaxed);
            mismatch += r.mismatch.load(std::memory_order_relaxed);
            known += r.knownDifferent.load(std::memory_order_relaxed);
            legacyOnly += r.legacyOnly.load(std::memory_order_relaxed);
            noWriter += r.noWriter.load(std::memory_order_relaxed);
        }
        char buffer[256];
        std::snprintf(buffer, sizeof(buffer), "writer shadow: %u seen, %u exact, %u mismatched, %u known-different, %u legacy-only, %u no-writer",
                      seen, exact, mismatch, known, legacyOnly, noWriter);
        line(buffer);
        for (size_t i = 0; i < kRows; ++i)
        {
            Row const& r = m_rows[i];
            if (!r.seen.load(std::memory_order_relaxed)) { continue; }
            char detail[96] = "";
            const uint32 first = r.firstDifference.load(std::memory_order_relaxed);
            if (first == 0xFFFFFFFFu)
            {
                std::snprintf(detail, sizeof(detail), " (kernel opcode 0x%04X)", r.otherOpcode.load(std::memory_order_relaxed));
            }
            else if (first)
            {
                std::snprintf(detail, sizeof(detail), " (first at byte %u, kernel 0x%04X)", first - 1, r.otherOpcode.load(std::memory_order_relaxed));
            }
            if (i == kRows - 1)
            {
                std::snprintf(buffer, sizeof(buffer), "  (no matrix row) last 0x%04X: seen %u, no-writer %u",
                              r.otherOpcode.load(std::memory_order_relaxed), r.seen.load(std::memory_order_relaxed), r.noWriter.load(std::memory_order_relaxed));
            }
            else
            {
                MatrixRow const& row = MatrixRowAt(i / kForms);
                const bool spline = (i % kForms) == 1;
                std::snprintf(buffer, sizeof(buffer), "  %s%s %s 0x%04X: seen %u, exact %u, mismatched %u%s, known-different %u, legacy-only %u, no-writer %u",
                              ChangeName(row.type), row.apply ? "+" : "-", spline ? "spline" : "mover", spline ? row.spline : row.mover,
                              r.seen.load(std::memory_order_relaxed), r.exact.load(std::memory_order_relaxed), r.mismatch.load(std::memory_order_relaxed), detail,
                              r.knownDifferent.load(std::memory_order_relaxed), r.legacyOnly.load(std::memory_order_relaxed), r.noWriter.load(std::memory_order_relaxed));
            }
            line(buffer);
        }
    }
}
