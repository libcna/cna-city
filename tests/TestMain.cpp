// SPDX-License-Identifier: MIT
//
// The cna-city test suite.
//
// The program is a benchmark, and a benchmark whose correctness is unverified measures nothing:
// a simulation that quietly stops moving anybody gets *faster*, and every one of the defects
// recorded in ARCHITECTURE.md did exactly that. Two of them -- pedestrians that never arrived and
// citizens that never left home -- looked like tuning, and one -- half the population no longer
// making decisions -- looked like an improvement.
//
// So the suite is weighted towards regression rather than towards unit coverage. Every test that
// names a defect in its own name is one that was found by looking at a screenshot, and none of
// them would have needed a screenshot if this file had existed.

#include <gtest/gtest.h>

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
