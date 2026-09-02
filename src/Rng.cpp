// SPDX-License-Identifier: MIT
#include "Rng.hpp"

#include <cmath>

namespace CnaCity
{
    float Rng::SqrtNegTwoLogOver(float s)
    {
        return std::sqrt(-2.0f * std::log(s) / s);
    }
}
