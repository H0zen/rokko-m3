// P0-D: the random generator behind urand can be seeded; the harness seeds it per scenario.
#include "TestHarness.h"
#include "RNGen.h"
#include "Util.h"

#include <vector>

TEST(RNG_seed_reproduces_the_sequence)
{
    std::vector<uint32> first;
    RNG::Seed(7);
    for (int i = 0; i < 16; ++i)
    {
        first.push_back(urand(0, 1000000));
    }
    std::vector<uint32> second;
    RNG::Seed(7);
    for (int i = 0; i < 16; ++i)
    {
        second.push_back(urand(0, 1000000));
    }
    CHECK(first == second);

    std::vector<uint32> other;
    RNG::Seed(8);
    for (int i = 0; i < 16; ++i)
    {
        other.push_back(urand(0, 1000000));
    }
    CHECK(first != other);
}
