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

// The jump arc's arithmetic (live test 2026-09-22, B4 "Heroic Leap travels a straight path").
// Pure: no unit, no spline, no DBC -- the three yardsticks are the client's own elevation
// formula (MoveSpline.cpp:137), the three leaps decoded out of mvcapture.log
// (peer/live-test-decode-2026-09-22.md section 2) and spell 6544's SpellEffect.dbc row.

#include "TestHarness.h"
#include "movement/JumpArc.h"

#include <cmath>

using namespace Movement::JumpArc;

namespace
{
    /// The client's gravity (Movement::gravity, src/game/movement/util.cpp:61), written out
    /// rather than linked: this file is pure and util.cpp is not in the test target.
    const float kGravity = 19.29110527038574f;

    bool Near(float a, float b, float tol = 0.001f) { return std::fabs(a - b) <= tol; }

    /// MoveSpline's own rendering, sampled: the chord plus the bow, as
    /// MoveSpline::computeParabolicElevation adds it. Returns the greatest height above the
    /// launch point over the leg, which is what a player sees as "the arc".
    float SampledApex(float amplitude, float deltaZ, float duration, int steps)
    {
        const float a = amplitude * 8.0f / (duration * duration);
        float best = 0.0f;
        for (int i = 0; i <= steps; ++i)
        {
            const float t = duration * float(i) / float(steps);
            const float z = deltaZ * (t / duration) + (duration - t) * 0.5f * a * t;
            if (z > best) { best = z; }
        }
        return best;
    }
}

TEST(JumpArc_ApexMatchesTheClientsOwnElevationFormula)
{
    // The closed form is only worth anything if it agrees with what MoveSpline draws. Three
    // shapes, each sampled at 20 000 points of the real elevation expression, at two very
    // different durations to show the apex does not depend on one.
    const float cases[][2] = { { 2.5f, 0.0f }, { 2.5f, -1.2f }, { 7.366f, -12.3f }, { 2.5f, 8.31f } };
    for (int k = 0; k < 4; ++k)
    {
        const float h = cases[k][0], dz = cases[k][1];
        CHECK(Near(ApexOfAmplitude(h, dz), SampledApex(h, dz, 0.5f, 20000), 0.01f));
        CHECK(Near(ApexOfAmplitude(h, dz), SampledApex(h, dz, 2.5f, 20000), 0.01f));
    }
}

TEST(JumpArc_TheFixedAmplitudeReproducesTheCapturedLeaps)
{
    // peer/live-test-decode-2026-09-22.md section 2.2, the three Heroic Leaps of PLAYER(low=25),
    // every one of them carrying the hard-coded 2.5 f: the flat 33 yd leap peaked 1.94 yd above
    // the launch, the uphill one 8.38, and the 12.3 yd downhill one NEVER ROSE. Those are the
    // numbers the fix has to beat, and this is the arithmetic that produced them.
    CHECK(Near(ApexOfAmplitude(2.5f, -1.20f), 1.936f, 0.005f));
    CHECK(Near(ApexOfAmplitude(2.5f, 8.31f), 8.381f, 0.005f));
    CHECK_EQ(ApexOfAmplitude(2.5f, -12.30f), 0.0f);

    // And the threshold behind that zero: a descent rises only while amplitude > |deltaZ|/4.
    CHECK_EQ(ApexOfAmplitude(3.0f, -12.0f), 0.0f);
    CHECK(ApexOfAmplitude(3.1f, -12.0f) > 0.0f);
}

TEST(JumpArc_AmplitudeDeliversTheApexItWasAskedFor)
{
    // The solve, round-tripped over a grid of clearances and slopes -- including slopes far
    // steeper than either endpoint of the capture, and the uphill side where the apex is
    // raised to sit above the HIGHER end.
    const float apexes[] = { 0.5f, 2.5f, 3.15f, 10.0f };
    const float slopes[] = { -40.0f, -12.3f, -1.2f, 0.0f, 1.2f, 8.31f, 40.0f };
    for (int i = 0; i < 4; ++i)
    {
        for (int j = 0; j < 7; ++j)
        {
            const float want = apexes[i] + (slopes[j] > 0.0f ? slopes[j] : 0.0f);
            const float h = AmplitudeForApex(apexes[i], slopes[j]);
            CHECK(h > 0.0f);
            CHECK(Near(ApexOfAmplitude(h, slopes[j]), want, 0.002f * (1.0f + want)));
        }
    }
}

TEST(JumpArc_EveryDescentRises)
{
    // The whole point of the change: on a downhill leap the character must go UP before it
    // goes down, which needs amplitude > |deltaZ|/4 -- 3.08 yd on the capture's -12.3 leap,
    // where 2.5 was passed.
    const float slopes[] = { -0.5f, -1.2f, -5.0f, -12.3f, -30.0f, -80.0f };
    for (int j = 0; j < 6; ++j)
    {
        const float h = AmplitudeForApex(2.5f, slopes[j]);
        CHECK(h > -slopes[j] / 4.0f);
        CHECK(Near(ApexOfAmplitude(h, slopes[j]), 2.5f, 0.01f));
    }
}

TEST(JumpArc_TheFlightHeightComesFromTheSpellsOwnPair)
{
    // Spell 6544 (Heroic Leap), SpellEffect.dbc effect 1: EffectMiscValue 25, EffectMiscValueB
    // 100 -- 2.5 yd and 10 yd. Below 1.018 s the floor wins (which is why the 2.5 f that was
    // hard-coded here looked right on a short leap and wrong on every other), above 2.036 s the
    // ceiling does, and between them the arc is the natural gravity one.
    const float minH = 2.5f, maxH = 10.0f;
    CHECK(Near(ApexForFlight(minH, maxH, 0.0f, kGravity), 2.5f));
    CHECK(Near(ApexForFlight(minH, maxH, 0.9558f, kGravity), 2.5f));          // the capture's 33 yd leap
    CHECK(Near(ApexForFlight(minH, maxH, 40.0f / 35.0f, kGravity), 3.1497f, 0.001f));  // a 40 yd leap at 35 yd/s
    CHECK(Near(ApexForFlight(minH, maxH, 3.0f, kGravity), 10.0f));           // capped
    // A pair the DBC does not carry (SPELL_EFFECT_PLAYER_PULL) falls back to the plain
    // gravity arc between 0.5 and 1000.
    CHECK(Near(ApexForFlight(0.5f, 1000.0f, 1.0f, kGravity), kGravity / 8.0f, 0.001f));
    CHECK(Near(ApexForFlight(0.5f, 1000.0f, 0.1f, kGravity), 0.5f));
}

TEST(JumpArc_TheWholeLegSolvedAtOnce)
{
    // AmplitudeForLeg, on the three leaps of the capture at spell 6544's own Spell.dbc speed
    // of 35 yd/s -- the numbers the fix puts on the wire in place of 2.500 every time.
    const float minH = 2.5f, maxH = 10.0f, speed = 35.0f;

    // 33.43 yd out, 1.20 down: 33.452 of leg, 0.9558 s, floor apex 2.5, and the rise goes from
    // 1.94 to the 2.50 that was asked for.
    const float flat = AmplitudeForLeg(minH, maxH, 33.452f, speed, -1.20f, kGravity);
    CHECK(Near(flat, 3.071f, 0.005f));
    CHECK(Near(ApexOfAmplitude(flat, -1.20f), 2.5f, 0.01f));

    // 22.85 yd out, 12.30 down: 25.949 of leg, 0.7414 s -- 0.00 of rise before, 2.50 after.
    const float down = AmplitudeForLeg(minH, maxH, 25.949f, speed, -12.30f, kGravity);
    CHECK(Near(down, 7.366f, 0.005f));
    CHECK(Near(ApexOfAmplitude(down, -12.30f), 2.5f, 0.01f));

    // A 40 yd leap on the level: 1.1429 s puts the natural arc above the floor, so the apex
    // scales with the leap at last instead of sitting at 2.5 for every distance.
    const float far_ = AmplitudeForLeg(minH, maxH, 40.0f, speed, 0.0f, kGravity);
    CHECK(Near(far_, 3.1497f, 0.001f));
    CHECK(Near(ApexOfAmplitude(far_, 0.0f), 3.1497f, 0.005f));

    // A standing-still leg cannot divide by a speed of zero, and must still hand back a usable
    // bow rather than a NaN.
    const float nospeed = AmplitudeForLeg(minH, maxH, 10.0f, 0.0f, 0.0f, kGravity);
    CHECK(Near(nospeed, 2.5f, 0.001f));
}
