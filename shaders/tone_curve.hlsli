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

// Hajime Uchimura's tone curve for Gran Turismo Sport (CEDEC 2017, "HDR Theory and Practice") with its published
// defaults: a toe (power 1.33 below 0.22), a linear middle (slope 1) and an exponential shoulder that approaches the peak
// without reaching it. The shoulder starts at UchimuraShoulder(peak): 0.53 for an SDR display, where white shows at 0.83;
// 2.1 paper whites on a display with 5 times paper white's peak, where white stays white.
static const float kUchimuraContrast = 1.0;
static const float kUchimuraLinearStart = 0.22;
static const float kUchimuraLinearLength = 0.4;
static const float kUchimuraToe = 1.33;

float UchimuraShoulder(float peak)
{
    return kUchimuraLinearStart + (peak - kUchimuraLinearStart) * kUchimuraLinearLength / kUchimuraContrast;
}

float3 Uchimura(float3 x, float peak)
{
    const float a = kUchimuraContrast;
    const float m = kUchimuraLinearStart;
    x = max(x, 0.0);
    float l0 = (peak - m) * kUchimuraLinearLength / a;
    float s0 = m + l0;
    float s1 = m + a * l0;
    float c2 = a * peak / (peak - s1);
    float cp = -c2 / peak;
    float3 w0 = 1.0 - smoothstep(0.0, m, x);
    float3 w2 = step(s0, x);
    float3 w1 = 1.0 - w0 - w2;
    float3 toe = m * pow(x / m, kUchimuraToe);
    float3 shoulder = peak - (peak - s1) * exp(cp * (x - s0));
    float3 linearPart = m + a * (x - m);
    return toe * w0 + linearPart * w1 + shoulder * w2;
}

// Filmic, on SDR and HDR displays alike: Uchimura's curve keeping the hue. The colour is scaled by the curve of its
// brightest channel, so no channel passes the peak and hues hold where the curve per channel turns orange yellow; into
// the shoulder it blends toward the curve per channel (as RolledOff desaturates), so the brightest colours go white as
// film does. Our choice: Gran Turismo applies the curve per channel.
float3 Filmic(float3 color, float peak)
{
    float brightest = max(color.r, max(color.g, color.b));
    if (brightest <= 0.0)
        return 0.0;
    float3 hueKept = color * (Uchimura(brightest.xxx, peak).x / brightest);
    float shoulder = UchimuraShoulder(peak);
    float x = max(brightest - shoulder, 0.0) / (peak - shoulder);
    return lerp(hueKept, Uchimura(color, peak), x / (x + 4.0));
}

float3 ToneCurve(float3 color, uint curve, float highlightRolloff, float peak)
{
    color = max(color, 0.0);
    if (curve == kToneCurveFilmic)
        return Filmic(color, peak);
    if (curve == kToneCurveHighlights)
        color = RolledOff(color, highlightRolloff, peak);
    return min(color, peak);
}

// In HDR, the nits of the frame's 1, the HUD's and the menu's white: `uiWhite`, or paper white where it is 0.
float UiWhite(float paperWhite, float uiWhite)
{
    return uiWhite > 0.0 ? uiWhite : paperWhite;
}

#endif
