#pragma once

#include <vector>

namespace gmodpp::native::lens_flare {

// A lens flare ghost: the image of the aperture after two reflections inside the lens, as the flare shaders draw it
// (FlareGhost in shaders/post/flare.hlsl), in the view's pixels. Per colour (red, green, blue): its centre and the radius
// of the aperture's circumscribed circle, negative where the image is turned over. Its brightness per colour relative to
// a white surface facing the light, and the circle the lens's front element lets light through within.
struct Ghost {
    float centre[3][2];
    float radius[3];
    float tint[3];
    float circleCentre[2];
    float circleRadius;
    float padding;
};
static_assert(sizeof(Ghost) == 64);

// The camera the ghosts are seen through: the view's size and focal length in pixels, the light's position on screen,
// the f-number, how strongly the glass disperses colours (1: as the glass does) and the flare's brightness.
struct View {
    float width;
    float height;
    float focalPixels;
    float lightX;
    float lightY;
    float fNumber;
    float dispersion;
    float intensity;
};

// The ghosts of every pair of surfaces of the lens for a light at infinity seen at `view.light`, without those off
// screen or too faint to see.
std::vector<Ghost> Ghosts(const View& view);

// Logs the lens's paraxial focal length and back focus, which should be its design's 1.
void LogPrescription();

}
