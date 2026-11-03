#pragma once

#include <array>

namespace gmodpp {

// The colour of a black body's light at `kelvin` (Kim et al. 2002's cubic spline of the Planckian locus, 1667 K to
// 25000 K, clamped to it) as linear sRGB (IEC 61966-2-1) of luminance 1.
std::array<float, 3> BlackbodyColor(float kelvin);

}
