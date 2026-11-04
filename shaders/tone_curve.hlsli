#ifndef GMODPP_TONE_CURVE_HLSLI
#define GMODPP_TONE_CURVE_HLSLI

// How lit colour after the game's tonemap scale is fitted into an 8-bit target, which clips each channel at 1
// (Settings::toneCurve): not at all, by rolling off the highlights alone, or by a filmic curve over the whole range.
static const uint kToneCurveOff = 0;
static const uint kToneCurveHighlights = 1;
static const uint kToneCurveFilmic = 2;

// Above a knee the luminance follows a shoulder that never reaches white (Reinhard's x / (1 + x) above the knee,
// continuous in value and slope), keeping its hue and desaturating toward white as it rises, as film does; below the
// knee nothing changes. `strength` blends from the clipped colour (0) to the full shoulder (1).
float3 RolledOff(float3 color, float strength)
{
    const float kKnee = 0.75;
    float luminance = dot(color, float3(0.2126, 0.7152, 0.0722));
    if (luminance <= kKnee || strength <= 0.0)
        return color;
    float x = (luminance - kKnee) / (1.0 - kKnee);
    float rolled = kKnee + (1.0 - kKnee) * x / (1.0 + x);
    float3 compressed = lerp(color * (rolled / luminance), rolled.xxx, x / (x + 4.0));
    return lerp(color, compressed, strength);
}

// Troy Sobotka's AgX as Blender ships it, in Rec. 2020 primaries (the inset and outset matrices of Eary Chow's
// AgX_LUT_Gen, AgXBaseRec2020.py), with Benjamin Wrensch's polynomial fit of its sigmoid, as Filament's AgxToneMapper
// and three.js's AgXToneMapping implement it: log2 encoding from 10 stops below middle grey to 6.5 above, the sigmoid, a
// power of 2.2 back to linear. Middle grey (0.18) shows at 0.21, 1 at 0.59, 16 reaches white, and bright colours
// desaturate toward white instead of clipping channel by channel.
float3 AgX(float3 color)
{
    // ITU-R BT.2407 (linear sRGB to and from linear Rec. 2020).
    static const float3x3 kSrgbToRec2020 = {
        0.6274, 0.3293, 0.0433,
        0.0691, 0.9195, 0.0113,
        0.0164, 0.0880, 0.8956};
    static const float3x3 kRec2020ToSrgb = {
        1.6605, -0.5876, -0.0728,
        -0.1246, 1.1329, -0.0083,
        -0.0182, -0.1006, 1.1187};
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

float3 ToneCurve(float3 color, uint curve, float highlightRolloff)
{
    if (curve == kToneCurveFilmic)
        return AgX(color);
    if (curve == kToneCurveHighlights)
        return RolledOff(color, highlightRolloff);
    return color;
}

#endif
