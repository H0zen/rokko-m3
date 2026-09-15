// P0-D: the process clock behind getMSTime(). Real mode reads the steady clock; a
// stepped run moves the counter by hand; leaving keeps an offset so time never runs
// backwards. Nothing here touches a map or the world.
#include "TestHarness.h"
#include "WorldClock.h"
#include "Timer.h"

#include <atomic>
#include <chrono>
#include <ctime>
#include <thread>

namespace
{
    // A test that enters stepped mode must leave it again before the next test runs,
    // even if a REQUIRE above returns early; CHECK alone never does, but this is cheap
    // insurance against the one macro that does.
    struct SteppedModeGuard
    {
        SteppedModeGuard() { WorldClock::EnterStepped(); }
        ~SteppedModeGuard() { WorldClock::LeaveStepped(); }
    };
}

TEST(WorldClock_stepped_time_moves_only_by_Step)
{
    const uint32 before = WorldClock::NowMs();
    WorldClock::EnterStepped();
    const uint32 start = WorldClock::NowMs();
    CHECK(start >= before);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK_EQ(WorldClock::NowMs(), start);                 // real time passed, the counter did not
    WorldClock::Step(50);
    WorldClock::Step(50);
    WorldClock::Step(50);
    CHECK_EQ(WorldClock::NowMs(), start + 150);
    CHECK_EQ(getMSTime(), start + 150);                   // getMSTime is the same source
    CHECK(WorldClock::IsStepped());
    WorldClock::LeaveStepped();
    CHECK(!WorldClock::IsStepped());
}

TEST(WorldClock_leaving_never_runs_backwards)
{
    WorldClock::EnterStepped();
    const uint32 start = WorldClock::NowMs();
    for (int i = 0; i < 200; ++i)                         // ten virtual seconds in no real time
    {
        WorldClock::Step(50);
    }
    const uint32 virt = WorldClock::NowMs();
    CHECK_EQ(virt, start + 10000);
    WorldClock::LeaveStepped();
    const uint32 after = WorldClock::NowMs();
    CHECK(after >= virt);                                 // the offset carries the counter's lead
    CHECK(WorldClock::OffsetMs() >= 9000);                // ten seconds minus whatever the loop took
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(WorldClock::NowMs() >= after);
    const uint32 beforeSecondLeave = WorldClock::NowMs();
    WorldClock::LeaveStepped();                           // a second leave while real: nothing moves backwards
    CHECK(WorldClock::NowMs() >= beforeSecondLeave);
    CHECK(WorldClock::OffsetMs() >= 9000);
}

TEST(WorldClock_unix_seconds_follow_the_steps_then_step_back_to_the_wall_clock)
{
    // Real mode, before entering: the wall clock, exactly.
    const time_t wallBefore = std::time(nullptr);
    const time_t before = WorldClock::NowUnix();
    CHECK(before >= wallBefore - 1);
    CHECK(before <= wallBefore + 1);

    // Stepped: the anchor plus the counter's advance, whatever the wall clock does.
    WorldClock::EnterStepped();
    const time_t s0 = WorldClock::NowUnix();
    WorldClock::Step(2000);                               // two virtual seconds
    CHECK_EQ(uint32(WorldClock::NowUnix() - s0), uint32(2));

    // Leaving steps the seconds back to the wall clock in one move; the ms clock
    // keeps the run's lead as before (OffsetMs(), not the seconds).
    WorldClock::LeaveStepped();
    const time_t wallAfter = std::time(nullptr);
    const time_t after = WorldClock::NowUnix();
    CHECK(after >= wallAfter - 1);
    CHECK(after <= wallAfter + 1);
    CHECK(WorldClock::OffsetMs() >= 1000);                // two seconds' step, safely above whatever real time this test took
}

TEST(WorldClock_another_thread_reading_while_stepped_never_sees_a_step_back)
{
    SteppedModeGuard guard;   // restores real mode even if a REQUIRE below returned early
    const uint32 beforeFirstStep = WorldClock::NowMs();

    std::atomic<bool> stop{false};
    std::atomic<bool> sawDecrease{false};
    std::atomic<uint32> minSeen{beforeFirstStep};
    std::atomic<uint32> maxSeen{beforeFirstStep};

    // Reads NowMs() in a loop for about 50 ms of real time -- steady_clock, not
    // WorldClock, since the clock under test is what is being read.
    std::thread spinner([&]
    {
        const auto spinUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
        uint32 last = WorldClock::NowMs();
        while (!stop.load(std::memory_order_acquire) || std::chrono::steady_clock::now() < spinUntil)
        {
            const uint32 now = WorldClock::NowMs();
            if (now < last)
            {
                sawDecrease.store(true, std::memory_order_relaxed);
            }
            last = now;

            uint32 curMin = minSeen.load(std::memory_order_relaxed);
            while (now < curMin && !minSeen.compare_exchange_weak(curMin, now, std::memory_order_relaxed)) {}
            uint32 curMax = maxSeen.load(std::memory_order_relaxed);
            while (now > curMax && !maxSeen.compare_exchange_weak(curMax, now, std::memory_order_relaxed)) {}
        }
    });

    // Three 50 ms ticks, a short real sleep between each so the spinner has a chance
    // to observe the value in between.
    uint32 lastStepped = beforeFirstStep;
    for (int i = 0; i < 3; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        WorldClock::Step(50);
        lastStepped = WorldClock::NowMs();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(15));   // let the spinner catch the last step

    stop.store(true, std::memory_order_release);
    spinner.join();

    CHECK(!sawDecrease.load());
    CHECK_EQ(maxSeen.load(), lastStepped);
    CHECK(minSeen.load() >= beforeFirstStep);
}
