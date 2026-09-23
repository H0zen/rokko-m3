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

#ifndef MANGOSSERVER_JUMPARC_H
#define MANGOSSERVER_JUMPARC_H

#include "Platform/Define.h"

#include <algorithm>
#include <cmath>

namespace Movement
{
    /**
     * THE ARC A JUMP SPELL ASKS FOR, AND THE NUMBER THE CLIENT ACTUALLY WANTS.
     *
     * `MoveSplineInit::SetParabolic(amplitude, 0)` does NOT send a height. It sends an
     * amplitude that `MoveSpline::Launch` turns into `vertical_acceleration = amplitude*8/T^2`
     * (MoveSpline.cpp:279), and `computeParabolicElevation` (MoveSpline.cpp:137) then ADDS
     *
     *     el(t) = (T - t) * 0.5 * a * t
     *
     * to the position the spline already interpolated -- so the amplitude is a BOW ABOUT THE
     * CHORD, not a height above the launch point and not a height above the ground. On a
     * two-point leg the chord is a straight line, so the rendered height is
     *
     *     z(t) = z0 + dz*(t/T) + 0.5*a*t*(T - t),   a = 8h/T^2
     *
     * whose peak above the launch point is, after T cancels,
     *
     *     apex(h, dz) = (dz + 4h)^2 / (16h)        while dz < 4h
     *
     * and whose launch-time vertical velocity is (dz + 4h)/T. Two consequences the live test
     * of 2026-09-22 caught on Heroic Leap, whose call site passed a FIXED 2.5 yd:
     *
     *   * a flat leap of 33.4 yd peaked 1.94 yd above the launch, not 2.5 -- the chord's own
     *     fall eats into the bow;
     *   * a downhill leap (dz = -12.3 yd) NEVER ROSE AT ALL: with h = 2.5 the launch velocity
     *     (dz + 4h)/T is negative, so the character was thrown downwards and the "arc" was a
     *     straight descent. Rising at all needs h > |dz|/4.
     *
     * So a jump is specified here by the apex it should reach ABOVE THE LAUNCH POINT, and the
     * amplitude is solved back out of it. `AmplitudeForApex` is that solve; it is exactly the
     * amplitude of the TRUE-GRAVITY parabola that rises `apex` and lands `deltaZ` away, and it
     * is independent of the spline's duration, which is why the rendered apex is the one that
     * was asked for however fast the leg runs.
     */
    namespace JumpArc
    {
        /**
         * @brief The peak rise above the launch point that `amplitude` actually produces.
         *
         * The inverse of AmplitudeForApex, and the reading a test or a verdict takes.
         *
         * @param amplitude The chord-relative bow handed to SetParabolic.
         * @param deltaZ    Destination Z minus launch Z.
         * @return The greatest height above the launch point on the rendered path, 0 when the
         *         path never rises (a descent whose bow the chord swallows whole).
         */
        inline float ApexOfAmplitude(float amplitude, float deltaZ)
        {
            if (amplitude <= 0.0f)
            {
                return std::max(0.0f, deltaZ);
            }
            if (deltaZ >= 4.0f * amplitude)
            {
                return deltaZ;      // monotone climb: the highest point is the destination
            }
            const float vT = deltaZ + 4.0f * amplitude;   // launch vertical velocity times T
            if (vT <= 0.0f)
            {
                return 0.0f;        // launched downwards -- it never rises
            }
            return vT * vT / (16.0f * amplitude);
        }

        /**
         * @brief The height above the launch point at a given fraction of the leg.
         *
         * The same curve as ApexOfAmplitude's, written against the fraction of the way along
         * rather than the time -- which for a two-point leg walked at a constant speed are the
         * same number. Substituting t = u*T into `deltaZ*(t/T) + 0.5*a*t*(T-t)` with
         * a = 8h/T^2 cancels T out entirely and leaves `deltaZ*u + 4h*u*(1-u)`, so a sampler
         * that knows only HOW FAR ALONG a unit is can say exactly how high it should be. That
         * is what the harness checks the rendered path against, because the server relocates a
         * spline-moved unit only once per POSITION_UPDATE_DELAY (400 ms, Unit.cpp:6974) and a
         * short leap's apex can fall between two of those.
         *
         * @param amplitude The chord-relative bow handed to SetParabolic.
         * @param deltaZ    Destination Z minus launch Z.
         * @param fraction  How far along the leg, 0 at the launch and 1 at the destination.
         * @return The height above the launch point there (negative below it).
         */
        inline float RiseAtFraction(float amplitude, float deltaZ, float fraction)
        {
            const float u = std::min(std::max(fraction, 0.0f), 1.0f);
            return deltaZ * u + 4.0f * amplitude * u * (1.0f - u);
        }

        /**
         * @brief The amplitude `SetParabolic` needs for a given apex above the launch point.
         *
         * Solving apex(h, dz) = P for h gives h = ((sqrt(P) + sqrt(P - dz)) / 2)^2, which is
         * the true-gravity parabola's own amplitude: rise time sqrt(2P/g), fall time
         * sqrt(2(P-dz)/g), and g*T^2/8 with that T is the expression above.
         *
         * `apex` is measured from the launch point, so it is raised by an uphill deltaZ first:
         * a leap onto a ledge 8 yd up cannot peak 2.5 yd above the launch, and what the caller
         * means there is 2.5 yd of clearance over the HIGHER end. That also keeps the square
         * root real for every deltaZ.
         *
         * @param apex   The wanted clearance above the higher of the two endpoints, > 0.
         * @param deltaZ Destination Z minus launch Z.
         * @return The amplitude to hand to MoveJump / SetParabolic, always > 0.
         */
        inline float AmplitudeForApex(float apex, float deltaZ)
        {
            const float clearance = std::max(apex, 0.01f);
            const float peak = clearance + std::max(0.0f, deltaZ);
            const float up = std::sqrt(peak);
            const float down = std::sqrt(peak - deltaZ);
            const float half = (up + down) * 0.5f;
            return half * half;
        }

        /**
         * @brief The clearance a jump spell's own data asks for over a flight of `duration`.
         *
         * 4.3.4's SpellEffect.dbc parameterises SPELL_EFFECT_JUMP/JUMP_DEST with a pair of
         * heights in tenths of a yard -- NOT with speeds: EffectMiscValue is the floor and
         * EffectMiscValueB the ceiling, and between them the arc is the natural gravity one,
         * g*T^2/8. (Heroic Leap, spell 6544, effect 1: MiscValue 25, MiscValueB 100 -- 2.5 yd
         * and 10 yd. The 2.5 f the call site used to hard-code was this spell's FLOOR, spelled
         * into C++ and applied to every jump spell there is.)
         *
         * @param minHeight The floor, in yards.
         * @param maxHeight The ceiling, in yards.
         * @param duration  The flight time, in seconds.
         * @param gravity   The client's gravity, in yards per second squared.
         * @return The clearance to pass to AmplitudeForApex.
         */
        inline float ApexForFlight(float minHeight, float maxHeight, float duration, float gravity)
        {
            const float natural = gravity * duration * duration / 8.0f;
            return std::min(std::max(natural, minHeight), std::max(minHeight, maxHeight));
        }

        /// The whole derivation for one leg: the spell's height pair against the flight this
        /// leg will actually run, solved into the amplitude MoveJump takes.
        /// @param minHeight The floor in yards (EffectMiscValue/10, or 0.5 where there is none).
        /// @param maxHeight The ceiling in yards (EffectMiscValueB/10, or 1000 where there is none).
        /// @param length    The leg's 3D length in yards -- the spline's own, so the duration is exact.
        /// @param speed     The leg's velocity in yards per second.
        /// @param deltaZ    Destination Z minus launch Z.
        /// @param gravity   The client's gravity.
        inline float AmplitudeForLeg(float minHeight, float maxHeight, float length, float speed,
                                     float deltaZ, float gravity)
        {
            const float duration = speed > 0.0f ? length / speed : 0.0f;
            return AmplitudeForApex(ApexForFlight(minHeight, maxHeight, duration, gravity), deltaZ);
        }
    }
}

#endif // MANGOSSERVER_JUMPARC_H
