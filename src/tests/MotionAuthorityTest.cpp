// The allowed-mover set (design v2 §7, F1): which units one client may move and
// the one it has selected. Pure; the session owns one and the handlers ask it.
#include "TestHarness.h"
#include "Authority.h"

using namespace Motion;

namespace
{
    const uint64 kPlayer  = 0x0400000000000001ULL;
    const uint64 kVehicle = 0xF150000000000002ULL;
    const uint64 kOther   = 0x0400000000000003ULL;
}

TEST(MotionAuthority_starts_empty_and_lets_nothing_move_or_ack)
{
    Authority a;
    CHECK(a.Members().empty());
    CHECK_EQ(a.Selected(), uint64(0));
    CHECK(!a.MovesAs(kPlayer));
    CHECK(!a.MayAck(kPlayer));
    // A client can send guid 0; the guid == 0 guards are what refuse it.
    CHECK(!a.Deselect(0));
    CHECK(!a.MovesAs(0));
    CHECK_EQ(a.Counters().notActive, 2u);
    CHECK_EQ(a.Counters().notMember, 1u);
    CHECK_EQ(a.Counters().badDeselect, 1u);
}

TEST(MotionAuthority_add_makes_a_member_and_selects_it)
{
    Authority a;
    a.SetBase(kPlayer);
    a.Add(kPlayer);
    CHECK(a.IsMember(kPlayer));
    CHECK_EQ(a.Selected(), kPlayer);
    CHECK_EQ(a.Members().size(), size_t(1));
    CHECK_EQ(a.Counters().added, 1u);
    // A second member is added and becomes the selection: the base was selected.
    a.Add(kVehicle);
    CHECK(a.IsMember(kPlayer));
    CHECK(a.IsMember(kVehicle));
    CHECK_EQ(a.Selected(), kVehicle);
    CHECK_EQ(a.Members().size(), size_t(2));
    CHECK_EQ(a.Members()[0], kPlayer);
    CHECK_EQ(a.Members()[1], kVehicle);
    CHECK_EQ(a.Counters().added, 2u);
}

TEST(MotionAuthority_a_grant_takes_the_selection_from_the_body_only)
{
    Authority a;
    a.SetBase(kPlayer);
    a.Add(kPlayer);
    a.Add(kVehicle);              // selected = kVehicle
    // A re-grant of the body does not steal the selection from the controlled unit.
    a.Add(kPlayer);
    CHECK_EQ(a.Selected(), kVehicle);
    CHECK_EQ(a.Counters().added, 2u);
    CHECK_EQ(a.Members().size(), size_t(2));
    // Nor does a third member joining while a controlled unit is selected.
    a.Add(kOther);
    CHECK_EQ(a.Selected(), kVehicle);
    // Once the controlled unit is gone, the body is free to take the selection again.
    CHECK(a.Remove(kVehicle));
    CHECK_EQ(a.Selected(), uint64(0));
    a.Add(kPlayer);
    CHECK_EQ(a.Selected(), kPlayer);
}

TEST(MotionAuthority_remove_of_the_selected_member_clears_the_selection)
{
    Authority a;
    a.SetBase(kPlayer);
    a.Add(kPlayer);
    a.Add(kVehicle);              // the base was selected, so this selects kVehicle
    CHECK(a.Remove(kVehicle));
    CHECK(!a.IsMember(kVehicle));
    CHECK(a.IsMember(kPlayer));
    CHECK_EQ(a.Selected(), uint64(0));
    CHECK_EQ(a.Counters().removed, 1u);
    CHECK(!a.MovesAs(kPlayer));   // a member, not selected
}

TEST(MotionAuthority_remove_of_another_member_keeps_the_selection_and_of_a_stranger_counts_nothing)
{
    Authority a;
    a.Add(kPlayer);
    a.Add(kVehicle);
    a.Select(kPlayer);
    CHECK(a.Remove(kVehicle));
    CHECK_EQ(a.Selected(), kPlayer);
    CHECK(!a.Remove(kOther));
    CHECK_EQ(a.Counters().removed, 1u);
}

TEST(MotionAuthority_select_takes_a_member_and_refuses_a_stranger)
{
    Authority a;
    a.Add(kPlayer);
    a.Add(kVehicle);
    CHECK(a.Select(kPlayer));
    CHECK_EQ(a.Selected(), kPlayer);
    CHECK_EQ(a.Counters().selected, 1u);
    CHECK(!a.Select(kOther));
    CHECK_EQ(a.Selected(), kPlayer);
    CHECK_EQ(a.Counters().badSelect, 1u);
}

TEST(MotionAuthority_deselect_never_moves_the_selection_and_counts_a_stranger)
{
    Authority a;
    a.SetBase(kPlayer);
    a.Add(kPlayer);
    a.Add(kVehicle);              // selected = kVehicle
    // The selected unit itself: counted, but the selection is untouched.
    CHECK(a.Deselect(kVehicle));
    CHECK_EQ(a.Selected(), kVehicle);
    CHECK_EQ(a.Counters().deselected, 1u);
    // The base player, named as the unit the client stopped moving: benign too.
    CHECK(a.Deselect(kPlayer));
    CHECK_EQ(a.Counters().deselected, 2u);
    CHECK_EQ(a.Selected(), kVehicle);
    // The unit just removed is named next (a revoke's deselect of itself,
    // after Remove already cleared the selection): benign too.
    CHECK(a.Remove(kVehicle));
    CHECK_EQ(a.Selected(), uint64(0));
    CHECK(a.Deselect(kVehicle));
    CHECK_EQ(a.Counters().deselected, 3u);
    // A stranger is a bad deselect ...
    CHECK(!a.Deselect(kOther));
    CHECK_EQ(a.Counters().badDeselect, 1u);
    // ... and so is guid 0.
    CHECK(!a.Deselect(0));
    CHECK_EQ(a.Counters().badDeselect, 2u);
}

TEST(MotionAuthority_movement_needs_the_selected_unit_and_an_ack_needs_membership)
{
    Authority a;
    a.SetBase(kPlayer);
    a.Add(kPlayer);
    a.Add(kVehicle);              // the base was selected, so this selects kVehicle
    CHECK(a.MovesAs(kVehicle));
    CHECK(!a.MovesAs(kPlayer));
    CHECK(!a.MovesAs(kOther));
    CHECK(!a.MovesAs(0));
    CHECK_EQ(a.Counters().notActive, 3u);
    CHECK(a.MayAck(kPlayer));
    CHECK(a.MayAck(kVehicle));
    CHECK(!a.MayAck(kOther));
    CHECK_EQ(a.Counters().notMember, 1u);
}

TEST(MotionAuthority_clear_removes_every_member_and_unresolved_counts)
{
    Authority a;
    a.Add(kPlayer);
    a.Add(kVehicle);
    a.Unresolved();
    a.Clear();
    CHECK(a.Members().empty());
    CHECK_EQ(a.Selected(), uint64(0));
    CHECK_EQ(a.Counters().removed, 2u);
    CHECK_EQ(a.Counters().unresolved, 1u);
}
