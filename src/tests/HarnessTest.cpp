// The GM harness's pure parts (movement P0-C): the step timeline every scenario
// runs on, the verdict line, the generator-type names. Nothing here touches a map.
#include "TestHarness.h"
#include "Timeline.h"

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

TEST(HarnessTypeName_is_the_old_table)
{
    CHECK_STR(Harness::TypeName(0), "IDLE");
    CHECK_STR(Harness::TypeName(8), "POINT");
    CHECK_STR(Harness::TypeName(14), "FOLLOW");
    CHECK_STR(Harness::TypeName(15), "EFFECT");
    CHECK_STR(Harness::TypeName(3), "?");
    CHECK_STR(Harness::TypeName(99), "?");
}

TEST(HarnessDist2_is_planar)
{
    CHECK_EQ(Harness::Dist2(0.0f, 0.0f, 3.0f, 4.0f), 5.0f);
    CHECK_EQ(Harness::Dist2(-3122.6f, -261.3f, -3122.6f, -261.3f), 0.0f);
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
