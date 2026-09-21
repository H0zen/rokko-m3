// The GM harness's pure parts (movement P0-C): the step timeline every scenario
// runs on, the verdict line, and the rule the teardown classifies an owned player by.
// Nothing here touches a map.
#include "TestHarness.h"
#include "Timeline.h"
#include "Ownership.h"

#include <string>
#include <vector>

TEST(HarnessTimeline_runs_due_steps_in_offset_order_once)
{
    Harness::Timeline t;
    std::vector<int> order;
    t.At(500, [&order]() { order.push_back(2); });
    t.At(200, [&order]() { order.push_back(1); });
    t.At(500, [&order]() { order.push_back(3); });   // a tie keeps insertion order
    CHECK_EQ(t.Pending(), size_t(3));
    t.Advance(100);
    CHECK(order.empty());
    t.Advance(300);
    CHECK_EQ(order.size(), size_t(1));
    CHECK_EQ(order[0], 1);
    t.Advance(600);
    CHECK_EQ(order.size(), size_t(3));
    CHECK_EQ(order[1], 2);
    CHECK_EQ(order[2], 3);
    CHECK(t.Idle());
    t.Advance(900);
    CHECK_EQ(order.size(), size_t(3));
}

TEST(HarnessTimeline_a_step_scheduled_from_a_step_is_relative_to_that_moment)
{
    Harness::Timeline t;
    std::vector<uint32> firedAt;
    t.At(1000, [&t, &firedAt]()
    {
        firedAt.push_back(t.Now());
        t.At(300, [&t, &firedAt]() { firedAt.push_back(t.Now()); });
    });
    t.Advance(1000);
    CHECK_EQ(firedAt.size(), size_t(1));
    CHECK_EQ(t.Pending(), size_t(1));
    t.Advance(1200);
    CHECK_EQ(firedAt.size(), size_t(1));
    t.Advance(1300);
    CHECK_EQ(firedAt.size(), size_t(2));
    CHECK_EQ(firedAt[1], 1300u);
}

TEST(HarnessTimeline_a_late_tick_runs_every_step_it_passed)
{
    Harness::Timeline t;
    int n = 0;
    for (uint32 i = 1; i <= 16; ++i)
    {
        t.At(1700 + i * 200, [&n]() { ++n; });
    }
    t.Advance(1700);
    CHECK_EQ(n, 0);
    t.Advance(2500);   // 1900, 2100, 2300, 2500 due
    CHECK_EQ(n, 4);
    t.Advance(9000);
    CHECK_EQ(n, 16);
}

TEST(HarnessVerdictLine_has_the_old_shape)
{
    CHECK_STR(Harness::VerdictLine("jump-over-point", "B1=OK(jump completed server-side) | B5=OK(point leg resumed)").c_str(),
              "VERDICT jump-over-point B1=OK(jump completed server-side) | B5=OK(point leg resumed)");
}

TEST(HarnessDist2_is_planar)
{
    CHECK_EQ(Harness::Dist2(0.0f, 0.0f, 3.0f, 4.0f), 5.0f);
    CHECK_EQ(Harness::Dist2(-3122.6f, -261.3f, -3122.6f, -261.3f), 0.0f);
}

// The teardown's own rule: what a scenario still owns of a harness player by the time
// Runner::End reaches its record. Addresses stand in for the objects because that is
// exactly what the rule is about -- the question is asked where following the pointer
// would be a use-after-free, so nothing here may be more than an address.
namespace
{
    void const* const kOwnedPlayer = reinterpret_cast<void const*>(0x1000);
    void const* const kAnotherPlayer = reinterpret_cast<void const*>(0x2000);
}

TEST(HarnessOwnership_the_registered_object_is_the_one_we_own)
{
    CHECK(Harness::ClassifyOwnership(kOwnedPlayer, kOwnedPlayer) == Harness::Ownership::Held);
}

TEST(HarnessOwnership_nothing_registered_means_somebody_else_destroyed_him)
{
    // Map::Remove(player, true) -> Map::DeleteFromWorld unregisters and then deletes, so an
    // empty answer is the only sign the teardown gets that its Player* is now freed memory.
    CHECK(Harness::ClassifyOwnership(kOwnedPlayer, NULL) == Harness::Ownership::Destroyed);
}

TEST(HarnessOwnership_a_different_object_on_the_guid_is_not_ours_to_touch)
{
    // The harness hands out guids from one small reserved block and restarts at the bottom
    // of it for every scenario, so a later player can answer an earlier one's guid. Presence
    // is not identity: tearing THAT one down would be worse than the leak it avoided.
    // And this outcome says nothing about whether OUR player is still alive -- he may be, and
    // being updated every tick -- so the teardown frees nothing on the strength of it, not
    // even the session it allocated itself.
    CHECK(Harness::ClassifyOwnership(kOwnedPlayer, kAnotherPlayer) == Harness::Ownership::Replaced);
    // And the classification is not symmetric in some accidental way: swap the roles and it
    // is still the record's pointer that decides.
    CHECK(Harness::ClassifyOwnership(kAnotherPlayer, kOwnedPlayer) == Harness::Ownership::Replaced);
}

TEST(HarnessOwnership_an_empty_record_never_reads_as_held)
{
    // Unreachable through SpawnPlayer, which records only a player it built; pinned anyway,
    // because the one outcome that must never come out of a NULL record is the branch that
    // dereferences it.
    CHECK(Harness::ClassifyOwnership(NULL, NULL) == Harness::Ownership::Destroyed);
    CHECK(Harness::ClassifyOwnership(NULL, kOwnedPlayer) == Harness::Ownership::Replaced);
}

TEST(HarnessSeed_derives_from_the_base_and_the_order)
{
    CHECK_EQ(Harness::SeedFor(0x4D56, 0), uint32(0x4D56));
    CHECK_EQ(Harness::SeedFor(0x4D56, 25), uint32(0x4D56 + 25));
    CHECK_EQ(Harness::SeedFor(2, 25), uint32(27));
    CHECK(Harness::SeedFor(2, 25) != Harness::SeedFor(3, 25));
    CHECK(Harness::TickSeed(0x4D56, 3, 0) != Harness::TickSeed(0x4D56, 3, 50));
    CHECK(Harness::TickSeed(0x4D56, 3, 100) != Harness::TickSeed(0x4D56, 4, 100));
    CHECK_EQ(Harness::TickSeed(7, 2, 150), Harness::TickSeed(7, 2, 150));
    CHECK(Harness::StepSeed(0x4D56, 3, 100) != Harness::TickSeed(0x4D56, 3, 100));
    CHECK(Harness::StepSeed(0x4D56, 3, 0) != Harness::StepSeed(0x4D56, 3, 50));
    CHECK_EQ(Harness::StepSeed(7, 2, 150), Harness::StepSeed(7, 2, 150));
}
