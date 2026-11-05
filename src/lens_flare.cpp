#include "lens_flare.h"

#include "log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>

namespace gmodpp::native::lens_flare {

namespace {

constexpr double kPi = 3.14159265358979323846;

// A surface of the lens, or its aperture stop: its radius of curvature (positive with the centre behind it), the
// distance to the next, the glass after it (refractive index for the helium d line, 587.6 nm, and Abbe number; air has
// none) and how far from the axis light passes through it.
struct Element {
    double radius;
    double thickness;
    double index;
    double abbe;
    double semiAperture;
    bool stop;
};

// A double Gauss lens: the example of US patent 2,673,491 (A. W. Tronnier, Voigtlander, 1954; the design in Smith's
// "Modern Lens Design", p. 312), focal length 1, f/2, 6 elements in 4 groups with two cemented doublets around the stop.
// Radii, thicknesses, indices and Abbe numbers are the patent's; the front surface's and the stop's clear apertures
// (which the patent leaves out) are pbrt's scaling of Smith's (dgauss.50mm.dat: 25.2 and 17.1 mm across at 50 mm).
constexpr Element kLens[] = {
    {0.58950, 0.07510, 1.67125, 47.1, 0.252, false},
    {1.59051, 0.00235, 1.0, 0.0, 0.252, false},
    {0.38554, 0.08053, 1.67125, 47.1, 0.23, false},
    {0.81537, 0.06550, 1.69842, 30.1, 0.23, false},
    {0.25502, 0.10931, 1.0, 0.0, 0.18, false},
    {0.0, 0.09481, 1.0, 0.0, 0.171, true},
    {-0.28992, 0.02362, 1.60266, 38.4, 0.17, false},
    {0.81537, 0.12134, 1.65953, 57.0, 0.2, false},
    {-0.40771, 0.00376, 1.0, 0.0, 0.2, false},
    {8.74182, 0.06443, 1.71740, 48.1, 0.2, false},
    {-0.79459, 0.0, 1.0, 0.0, 0.2, false},
};
constexpr size_t kElements = std::size(kLens);
// The wavelengths in nanometres the red, green and blue images are traced at.
constexpr double kWavelengths[3] = {610.0, 550.0, 465.0};
// Air-glass surfaces carry a quarter-wave magnesium fluoride coating (index 1.38) tuned to a wavelength between
// kCoatingShortest and kCoatingShortest + kCoatingSpread nanometres, a different one on each surface, as multi-coated
// lenses spread them.
constexpr double kCoatingIndex = 1.38;
constexpr double kCoatingShortest = 450.0;
constexpr double kCoatingSpread = 200.0;
// The smallest ghost drawn in pixels (smaller ones keep their light, spread over it) and the faintest, relative to a
// white surface facing the light.
constexpr double kSmallestGhost = 1.5;
constexpr double kFaintestGhost = 1e-5;

// A paraxial ray transfer matrix acting on (height, angle).
struct Matrix {
    double a = 1.0;
    double b = 0.0;
    double c = 0.0;
    double d = 1.0;
};

Matrix operator*(const Matrix& x, const Matrix& y) { return {x.a * y.a + x.b * y.c, x.a * y.b + x.b * y.d, x.c * y.a + x.d * y.c, x.c * y.b + x.d * y.d}; }

Matrix Translate(double distance) { return {1.0, distance, 0.0, 1.0}; }

// Refraction from index `from` into `to` at a surface of radius `radius`.
Matrix Refract(double from, double to, double radius) { return {1.0, 0.0, -(to - from) / (to * radius), from / to}; }

// Reflection at a surface of radius `radius`, the reflected ray's travel taken as forward again (the lens unfolded).
Matrix Reflect(double radius) { return {1.0, 0.0, 2.0 / radius, 1.0}; }

// The glass's index at `wavelength` nanometres by Cauchy's equation through its d-line index and its Abbe number
// (n_F - n_C = (n_d - 1) / V), its dispersion scaled by `dispersion`.
double IndexAt(const Element& element, double wavelength, double dispersion)
{
    if (element.abbe <= 0.0)
        return element.index;
    constexpr double kF = 0.4861327;
    constexpr double kC = 0.6562725;
    constexpr double kD = 0.5875618;
    const double b = (element.index - 1.0) / element.abbe / (1.0 / (kF * kF) - 1.0 / (kC * kC));
    const double micrometres = wavelength * 1e-3;
    return element.index + dispersion * b * (1.0 / (micrometres * micrometres) - 1.0 / (kD * kD));
}

// The lens at one wavelength: each element's index after it, and the index before the first.
struct Indices {
    std::array<double, kElements> after;
};

Indices IndicesAt(double wavelength, double dispersion)
{
    Indices indices;
    for (size_t e = 0; e < kElements; ++e)
        indices.after[e] = kLens[e].stop ? (e > 0 ? indices.after[e - 1] : 1.0) : IndexAt(kLens[e], wavelength, dispersion);
    return indices;
}

double IndexBefore(const Indices& indices, size_t element) { return element > 0 ? indices.after[element - 1] : 1.0; }

// The reflectance at normal incidence of surface `element` at `wavelength`: a single quarter-wave coating where it
// meets air (thin-film interference of the coating's two faces), the bare Fresnel reflectance where two glasses are
// cemented.
double Reflectance(const Indices& indices, size_t element, double wavelength)
{
    const double before = IndexBefore(indices, element);
    const double after = indices.after[element];
    if (before > 1.0 && after > 1.0) {
        const double r = (after - before) / (after + before);
        return r * r;
    }
    const double glass = std::max(before, after);
    const double design = kCoatingShortest + kCoatingSpread * std::fmod(0.5 + 0.6180339887 * static_cast<double>(element), 1.0);
    const double r01 = (1.0 - kCoatingIndex) / (1.0 + kCoatingIndex);
    const double r12 = (kCoatingIndex - glass) / (kCoatingIndex + glass);
    // The coating's optical thickness is a quarter of the design wavelength: its two reflections differ in phase by
    // pi x design / wavelength.
    const double phase = std::cos(kPi * design / wavelength);
    return (r01 * r01 + r12 * r12 + 2.0 * r01 * r12 * phase) / (1.0 + r01 * r01 * r12 * r12 + 2.0 * r01 * r12 * phase);
}

// The matrices from the front surface to the sensor `sensor` units behind the last, and to the aperture stop where the
// path first meets it, for the direct path (first < 0) or the ghost reflected at element `first` and then `second`.
struct Path {
    Matrix sensor;
    std::optional<Matrix> aperture;
};

Path Trace(const Indices& indices, int first, int second, double sensor)
{
    Path path;
    Matrix& m = path.sensor;
    const auto atStop = [&](size_t e) {
        if (kLens[e].stop && !path.aperture)
            path.aperture = m;
    };
    const auto forward = [&](size_t e) {
        if (kLens[e].stop)
            atStop(e);
        else
            m = Refract(IndexBefore(indices, e), indices.after[e], kLens[e].radius) * m;
    };
    // Travelling back, the unfolded lens sees each surface mirrored.
    const auto backward = [&](size_t e) {
        if (kLens[e].stop)
            atStop(e);
        else
            m = Refract(indices.after[e], IndexBefore(indices, e), -kLens[e].radius) * m;
    };
    if (first < 0) {
        for (size_t e = 0; e < kElements; ++e) {
            if (e > 0)
                m = Translate(kLens[e - 1].thickness) * m;
            forward(e);
        }
    } else {
        const size_t i = static_cast<size_t>(first);
        const size_t j = static_cast<size_t>(second);
        for (size_t e = 0;; ++e) {
            if (e > 0)
                m = Translate(kLens[e - 1].thickness) * m;
            if (e == i) {
                m = Reflect(kLens[i].radius) * m;
                break;
            }
            forward(e);
        }
        for (size_t e = i; e-- > j;) {
            m = Translate(kLens[e].thickness) * m;
            if (e == j) {
                m = Reflect(-kLens[j].radius) * m;
                break;
            }
            backward(e);
        }
        for (size_t e = j + 1; e < kElements; ++e) {
            m = Translate(kLens[e - 1].thickness) * m;
            forward(e);
        }
    }
    m = Translate(kLens[kElements - 1].thickness + sensor) * m;
    return path;
}

// The lens focused at infinity in green: the sensor's distance behind the last surface and the focal length.
struct Focus {
    double sensor;
    double focalLength;
};

Focus FocusOf(const Indices& green)
{
    const Matrix lens = Trace(green, -1, -1, 0.0).sensor;
    const double sensor = -lens.a / lens.c;
    return {sensor, Trace(green, -1, -1, sensor).sensor.b};
}

size_t StopElement()
{
    for (size_t e = 0; e < kElements; ++e) {
        if (kLens[e].stop)
            return e;
    }
    return 0;
}

}

std::vector<Ghost> Ghosts(const View& view)
{
    std::array<Indices, 3> indices;
    for (size_t c = 0; c < 3; ++c)
        indices[c] = IndicesAt(kWavelengths[c], view.dispersion);
    const Focus focus = FocusOf(IndicesAt(kWavelengths[1], 0.0));
    if (!std::isfinite(focus.sensor) || std::fabs(focus.focalLength) < 1e-9)
        return {};
    // The stop opens to the f-number's entrance pupil as far as it can.
    const Path direct = Trace(indices[1], -1, -1, focus.sensor);
    const double pupilToStop = direct.aperture ? std::fabs(direct.aperture->a) : 1.0;
    const double stop = std::min(pupilToStop * focus.focalLength / (2.0 * std::max(static_cast<double>(view.fNumber), 0.1)), kLens[StopElement()].semiAperture);
    // Lengths on the sensor in the lens's units become pixels at the view's focal length.
    const double pixels = view.focalPixels / focus.focalLength;
    const double centreX = 0.5 * view.width;
    const double centreY = 0.5 * view.height;
    const double axisX = view.lightX - centreX;
    const double axisY = view.lightY - centreY;

    std::vector<Ghost> ghosts;
    std::array<std::array<double, kElements>, 3> reflectance = {};
    for (size_t c = 0; c < 3; ++c) {
        for (size_t e = 0; e < kElements; ++e)
            reflectance[c][e] = kLens[e].stop ? 0.0 : Reflectance(indices[c], e, kWavelengths[c]);
    }
    for (size_t i = 0; i < kElements; ++i) {
        for (size_t j = 0; j < i; ++j) {
            if (kLens[i].stop || kLens[j].stop)
                continue;
            Ghost ghost = {};
            bool valid = true;
            double brightest = 0.0;
            double extent = 0.0;
            for (size_t c = 0; c < 3 && valid; ++c) {
                const Path path = Trace(indices[c], static_cast<int>(i), static_cast<int>(j), focus.sensor);
                const Matrix& s = path.sensor;
                if (!path.aperture || std::fabs(path.aperture->a) < 1e-9) {
                    valid = false;
                    break;
                }
                const Matrix& a = *path.aperture;
                // A point p of the stop images to scale x p + offset x field on the sensor (Lee and Eisemann 2013).
                const double scale = s.a / a.a;
                const double along = (s.b - s.a * a.b / a.a) / focus.focalLength;
                double radius = scale * stop * pixels;
                // The light through the entrance pupil spread over the ghost, as a white surface facing the light
                // would show it: reflectances x (focal length / (scale x stop))^2.
                double tint = reflectance[c][i] * reflectance[c][j] * (focus.focalLength * focus.focalLength) / (scale * scale * stop * stop) * view.intensity;
                if (!std::isfinite(tint) || !std::isfinite(radius)) {
                    valid = false;
                    break;
                }
                if (std::fabs(radius) < kSmallestGhost) {
                    tint *= radius * radius / (kSmallestGhost * kSmallestGhost);
                    radius = radius < 0.0 ? -kSmallestGhost : kSmallestGhost;
                }
                ghost.centre[c][0] = static_cast<float>(centreX + along * axisX);
                ghost.centre[c][1] = static_cast<float>(centreY + along * axisY);
                ghost.radius[c] = static_cast<float>(radius);
                ghost.tint[c] = static_cast<float>(tint);
                brightest = std::max(brightest, tint);
                extent = std::max(extent, std::fabs(radius));
                if (c == 1) {
                    // The rays the front surface lets in land within this circle.
                    const double across = s.b / focus.focalLength;
                    ghost.circleCentre[0] = static_cast<float>(centreX + across * axisX);
                    ghost.circleCentre[1] = static_cast<float>(centreY + across * axisY);
                    ghost.circleRadius = static_cast<float>(std::fabs(s.a) * kLens[0].semiAperture * pixels);
                }
            }
            if (!valid || brightest < kFaintestGhost)
                continue;
            const float x = ghost.centre[1][0];
            const float y = ghost.centre[1][1];
            const float reach = static_cast<float>(extent);
            if (x + reach < 0.0f || y + reach < 0.0f || x - reach > view.width || y - reach > view.height)
                continue;
            const float dx = ghost.circleCentre[0] - x;
            const float dy = ghost.circleCentre[1] - y;
            if (std::sqrt(dx * dx + dy * dy) > ghost.circleRadius + reach)
                continue;
            ghosts.push_back(ghost);
        }
    }
    return ghosts;
}

void LogPrescription()
{
    const Indices green = IndicesAt(kWavelengths[1], 0.0);
    const Focus focus = FocusOf(green);
    const Matrix sensor = Trace(green, -1, -1, focus.sensor).sensor;
    log::Info("Lens flare: double Gauss focal length {:.4f}, back focus {:.4f} (design 1), sensor matrix A {:.2e}", focus.focalLength, focus.sensor, sensor.a);
}

}
