#include "color.h"

#include <algorithm>

namespace gmodpp {

std::array<float, 3> BlackbodyColor(float kelvin)
{
    const double t = 1.0 / std::clamp(static_cast<double>(kelvin), 1667.0, 25000.0);
    const double x = t >= 1.0 / 4000.0 ? ((-0.2661239e9 * t - 0.2343580e6) * t + 0.8776956e3) * t + 0.179910
                                       : ((-3.0258469e9 * t + 2.1070379e6) * t + 0.2226347e3) * t + 0.240390;
    const double y = t >= 1.0 / 2222.0 ? ((-1.1063814 * x - 1.34811020) * x + 2.18555832) * x - 0.20219683
                   : t >= 1.0 / 4000.0 ? ((-0.9549476 * x - 1.37418593) * x + 2.09137015) * x - 0.16748867
                                       : ((3.0817580 * x - 5.87338670) * x + 3.75112997) * x - 0.37001483;
    const double X = x / y;
    const double Z = (1.0 - x - y) / y;
    return {static_cast<float>(std::max(3.2406 * X - 1.5372 - 0.4986 * Z, 0.0)), static_cast<float>(std::max(-0.9689 * X + 1.8758 + 0.0415 * Z, 0.0)),
        static_cast<float>(std::max(0.0557 * X - 0.2040 + 1.0570 * Z, 0.0))};
}

}
