#ifndef GMODPP_TONE_CURVE_HLSLI
#define GMODPP_TONE_CURVE_HLSLI

// How the 3D view's light (after the game's tonemap scale, 1 = paper white) fits what the display shows, up to its peak
// in paper whites (1 in SDR) (Settings::toneCurve, applied by the camera effects' last pass, shaders/amd/lens.hlsl, before
// the HUD): clipped at the peak, by rolling off the highlights alone, or by a filmic curve over the whole range.
static const uint kToneCurveOff = 0;
static const uint kToneCurveHighlights = 1;
static const uint kToneCurveFilmic = 2;

// ITU-R BT.2407 (linear sRGB to and from linear Rec. 2020).
static const float3x3 kSrgbToRec2020 = {
    0.6274, 0.3293, 0.0433,
    0.0691, 0.9195, 0.0113,
    0.0164, 0.0880, 0.8956};
static const float3x3 kRec2020ToSrgb = {
    1.6605, -0.5876, -0.0728,
    -0.1246, 1.1329, -0.0083,
    -0.0182, -0.1006, 1.1187};

// Above a knee at 0.75 of the peak the luminance follows a shoulder that never reaches the peak (Reinhard's x / (1 + x)
// above the knee, continuous in value and slope), keeping its hue and desaturating toward white as it rises, as film
// does; below the knee nothing changes. `strength` blends from the clipped colour (0) to the full shoulder (1).
float3 RolledOff(float3 color, float strength, float peak)
{
    const float kKnee = 0.75;
    color /= peak;
    float luminance = dot(color, float3(0.2126, 0.7152, 0.0722));
    if (luminance <= kKnee || strength <= 0.0)
        return color * peak;
    float x = (luminance - kKnee) / (1.0 - kKnee);
    float rolled = kKnee + (1.0 - kKnee) * x / (1.0 + x);
    float3 compressed = lerp(color * (rolled / luminance), rolled.xxx, x / (x + 4.0));
    return lerp(color, compressed, strength) * peak;
}

// Troy Sobotka's AgX as Blender ships it, in Rec. 2020 primaries (the inset and outset matrices of Eary Chow's
// AgX_LUT_Gen, AgXBaseRec2020.py), with Benjamin Wrensch's polynomial fit of its sigmoid, as Filament's AgxToneMapper
// and three.js's AgXToneMapping implement it: log2 encoding from 10 stops below middle grey to 6.5 above, the sigmoid, a
// power of 2.2 back to linear. Middle grey (0.18) shows at 0.21, 1 at 0.59, 16 reaches white, and bright colours
// desaturate toward white instead of clipping channel by channel. SDR only: it has no form for a brighter peak.
float3 AgX(float3 color)
{
    static const float3x3 kInset = {
        0.856627153315983, 0.0951212405381588, 0.0482516061458583,
        0.137318972929847, 0.761241990602591, 0.101439036467562,
        0.11189821299995, 0.0767994186031903, 0.811302368396859};
    static const float3x3 kOutset = {
        1.1271005818144368, -0.11060664309660323, -0.016493938717834573,
        -0.1413297634984383, 1.157823702216272, -0.016493938717834257,
        -0.14132976349843826, -0.11060664309660294, 1.2519364065950405};
    const float kMinEv = -12.47393;
    const float kMaxEv = 4.026069;
    float3 x = mul(kInset, mul(kSrgbToRec2020, max(color, 0.0)));
    x = saturate((log2(max(x, 1e-10)) - kMinEv) / (kMaxEv - kMinEv));
    float3 x2 = x * x;
    float3 x4 = x2 * x2;
    x = 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
    x = pow(max(mul(kOutset, x), 0.0), 2.2);
    return saturate(mul(kRec2020ToSrgb, x));
}

// Hajime Uchimura's tone curve for Gran Turismo Sport (CEDEC 2017, "HDR Theory and Practice"), per channel, with its
// published defaults: a toe (power 1.33 below 0.22), a linear middle (slope 1) and an exponential shoulder that
// approaches the peak `peak` without reaching it. Filmic's curve for an HDR display.
float3 Uchimura(float3 x, float peak)
{
    const float a = 1.0;
    const float m = 0.22;
    const float l = 0.4;
    const float c = 1.33;
    x = max(x, 0.0);
    float l0 = (peak - m) * l / a;
    float s0 = m + l0;
    float s1 = m + a * l0;
    float c2 = a * peak / (peak - s1);
    float cp = -c2 / peak;
    float3 w0 = 1.0 - smoothstep(0.0, m, x);
    float3 w2 = step(s0, x);
    float3 w1 = 1.0 - w0 - w2;
    float3 toe = m * pow(x / m, c);
    float3 shoulder = peak - (peak - s1) * exp(cp * (x - s0));
    float3 linearPart = m + a * (x - m);
    return toe * w0 + linearPart * w1 + shoulder * w2;
}

float3 ToneCurve(float3 color, uint curve, float highlightRolloff, float peak)
{
    color = max(color, 0.0);
    if (curve == kToneCurveFilmic)
        return peak <= 1.0 ? AgX(color) : Uchimura(color, peak);
    if (curve == kToneCurveHighlights)
        color = RolledOff(color, highlightRolloff, peak);
    return min(color, peak);
}

// In HDR, the nits of the frame's 1, the HUD's and the menu's white: `uiWhite`, or where it is 0 paper white times the
// HDR exposure's `gain` (ExposureMain in shaders/post/post.hlsl), so they stay as bright beside the 3D view as in SDR.
float UiWhite(float paperWhite, float uiWhite, float gain)
{
    return uiWhite > 0.0 ? uiWhite : paperWhite * gain;
}

#endif
