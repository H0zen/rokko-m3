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

#include "TestHarness.h"

#include "Move/Selection.h"

using Move::Held;
using Move::Layer;
using Move::Policy;
using Move::Selection;

namespace
{
    /// The kinds, as the tree numbers them, so a test reads like the game it describes.
    /// Opaque to Selection itself -- that is the point of the wall.
    enum Kind : uint8_t
    {
        Idle = 0, Wander, Patrol, Follow, Chase, Point, FlyLand, Home,
        AssistRun, Distract, AssistDistract, Fear, Confused, Effect, Taxi
    };

    bool SelectedIs(const Selection& s, Layer expected)
    {
        Layer at = Layer::Count;
        return s.Selected(at) && at == expected;
    }
}

TEST(SelectionStartsEmptyAndSelectsNothing)
{
    Selection s;
    Layer at = Layer::Count;
    CHECK(!s.Selected(at));
    CHECK_EQ(int(s.Depth()), 0);
}

TEST(SelectionIsTheHighestLayerHeld)
{
    Selection s;
    s.Request(Layer::Default, Policy::Supersede, Wander);
    CHECK(SelectedIs(s, Layer::Default));

    s.Request(Layer::Combat, Policy::Supersede, Chase);
    CHECK(SelectedIs(s, Layer::Combat));

    // A lower layer arriving later does NOT become the answer. Priority is the whole
    // content of the type; the tree's own defect was running the last push instead.
    s.Request(Layer::Default, Policy::Supersede, Patrol);
    CHECK(SelectedIs(s, Layer::Combat));
}

// Two things cannot share a priority and still have one of them be the answer.
TEST(ALayerHoldsExactlyOneThing)
{
    Selection s;
    s.Request(Layer::Combat, Policy::Supersede, Chase, 7);
    s.Request(Layer::Combat, Policy::Supersede, Follow, 9);

    CHECK_EQ(int(s.Depth()), 1);
    CHECK_EQ(int(s.At(Layer::Combat).kind), int(Follow));
    CHECK_EQ(s.At(Layer::Combat).id, uint32_t(9));
}

// The case the whole piece exists for: a fear interrupts a chase, and when the fear ends
// the chase is running again without anyone having stored it somewhere.
TEST(SuspendLetsWhatIsBeneathComeBack)
{
    Selection s;
    s.Request(Layer::Combat, Policy::Supersede, Chase);
    s.Request(Layer::Control, Policy::Suspend, Fear);

    CHECK(SelectedIs(s, Layer::Control));
    CHECK(s.IsHeld(Layer::Combat));

    s.Finish(Layer::Control);
    CHECK(SelectedIs(s, Layer::Combat));
    CHECK_EQ(int(s.At(Layer::Combat).kind), int(Chase));
}

TEST(OverrideEmptiesEverythingBeneathAndNothingComesBack)
{
    Selection s;
    s.Request(Layer::Default, Policy::Supersede, Patrol);
    s.Request(Layer::Combat, Policy::Supersede, Chase);
    s.Request(Layer::Taxi, Policy::Override, Taxi);

    CHECK_EQ(int(s.Depth()), 1);
    CHECK(SelectedIs(s, Layer::Taxi));

    s.Finish(Layer::Taxi);
    Layer at = Layer::Count;
    CHECK(!s.Selected(at));
}

// Override reaches DOWN only. A fear does not dislodge a taxi, and asking a priority not
// to be one is the bug this direction prevents.
TEST(APolicyNeverReachesAboveItsOwnLayer)
{
    Selection s;
    s.Request(Layer::Taxi, Policy::Override, Taxi);
    s.Request(Layer::Control, Policy::Override, Fear);

    CHECK(s.IsHeld(Layer::Taxi));
    CHECK(SelectedIs(s, Layer::Taxi));
}

// The distinction between the two policies, stated as one test: the same stack, the same
// interruption, and only the policy differs.
TEST(SuspendAndOverrideDifferOnlyInWhatSurvivesBeneath)
{
    Selection suspended;
    suspended.Request(Layer::Default, Policy::Supersede, Patrol);
    suspended.Request(Layer::Combat, Policy::Supersede, Chase);
    suspended.Request(Layer::Scripted, Policy::Suspend, Point);

    Selection overridden;
    overridden.Request(Layer::Default, Policy::Supersede, Patrol);
    overridden.Request(Layer::Combat, Policy::Supersede, Chase);
    overridden.Request(Layer::Scripted, Policy::Override, Point);

    CHECK_EQ(int(suspended.Depth()), 3);
    CHECK_EQ(int(overridden.Depth()), 1);

    suspended.Finish(Layer::Scripted);
    overridden.Finish(Layer::Scripted);

    CHECK(SelectedIs(suspended, Layer::Combat));
    Layer at = Layer::Count;
    CHECK(!overridden.Selected(at));
}

// A scripted point with resumeCombat is the reason Policy is an argument rather than a
// property of the kind: the same Point arrives as Suspend or as Override.
TEST(TheSameKindCanArriveUnderEitherPolicy)
{
    Selection resuming;
    resuming.Request(Layer::Combat, Policy::Supersede, Chase);
    resuming.Request(Layer::Scripted, Policy::Suspend, Point);
    resuming.Finish(Layer::Scripted);
    CHECK(SelectedIs(resuming, Layer::Combat));

    Selection ending;
    ending.Request(Layer::Combat, Policy::Supersede, Chase);
    ending.Request(Layer::Scripted, Policy::Override, Point);
    ending.Finish(Layer::Scripted);
    Layer at = Layer::Count;
    CHECK(!ending.Selected(at));
}

// Supersede touches nothing but its own layer, which is what lets a chase replace a follow
// without disturbing the patrol that will run when combat ends.
TEST(SupersedeLeavesEveryOtherLayerAlone)
{
    Selection s;
    s.Request(Layer::Default, Policy::Supersede, Patrol);
    s.Request(Layer::Control, Policy::Suspend, Confused);
    s.Request(Layer::Combat, Policy::Supersede, Chase);

    CHECK_EQ(int(s.Depth()), 3);
    CHECK(s.IsHeld(Layer::Default));
    CHECK(s.IsHeld(Layer::Control));
    CHECK(SelectedIs(s, Layer::Control));
}

// Home, distract and effect end the moment anything else is requested. Which kinds those
// are is the game's vocabulary, so the caller expires them; the piece only has to make
// that one call enough.
TEST(ExpiringALayerIsTheSameAsFinishingIt)
{
    Selection s;
    s.Request(Layer::Combat, Policy::Supersede, Chase);
    s.Request(Layer::Scripted, Policy::Suspend, Home);

    s.Expire(Layer::Scripted);
    CHECK(SelectedIs(s, Layer::Combat));
    CHECK(!s.IsHeld(Layer::Scripted));
}

TEST(FinishingAnEmptyOrUnselectedLayerChangesNothing)
{
    Selection s;
    s.Request(Layer::Combat, Policy::Supersede, Chase);

    s.Finish(Layer::Taxi);
    s.Finish(Layer::Default);
    CHECK(SelectedIs(s, Layer::Combat));
    CHECK_EQ(int(s.Depth()), 1);
}

// Finishing something that is NOT selected leaves the selection where it was: a patrol
// underneath a fear ending does not promote anything.
TEST(FinishingBeneathTheSelectionDoesNotMoveIt)
{
    Selection s;
    s.Request(Layer::Default, Policy::Supersede, Patrol);
    s.Request(Layer::Control, Policy::Suspend, Fear);

    s.Finish(Layer::Default);
    CHECK(SelectedIs(s, Layer::Control));
    CHECK_EQ(int(s.Depth()), 1);
}

TEST(ClearEmptiesEverything)
{
    Selection s;
    for (uint8_t i = 0; i < Selection::LAYERS; ++i)
    {
        s.Request(Layer(i), Policy::Supersede, uint8_t(i));
    }
    CHECK_EQ(int(s.Depth()), int(Selection::LAYERS));

    s.Clear();
    Layer at = Layer::Count;
    CHECK(!s.Selected(at));
    CHECK_EQ(int(s.Depth()), 0);
}

// The stack cannot grow: there are as many slots as layers and no more, however many
// requests arrive. That is the property that makes the whole thing seven bytes of state.
TEST(TheStackCannotGrowPastItsLayers)
{
    Selection s;
    for (int round = 0; round < 500; ++round)
    {
        s.Request(Layer::Default, Policy::Supersede, Wander);
        s.Request(Layer::Combat, Policy::Supersede, Chase);
        s.Request(Layer::Control, Policy::Suspend, Fear);
        s.Finish(Layer::Control);
    }
    CHECK(s.Depth() <= Selection::LAYERS);
    CHECK(SelectedIs(s, Layer::Combat));
}

// An out-of-range layer is refused rather than written past the end of the array.
TEST(SelectionRefusesALayerThatDoesNotExist)
{
    Selection s;
    s.Request(Layer::Count, Policy::Supersede, Chase);
    CHECK_EQ(int(s.Depth()), 0);
    CHECK(!s.IsHeld(Layer::Count));
    s.Finish(Layer::Count);
    CHECK_EQ(int(s.Depth()), 0);
}
