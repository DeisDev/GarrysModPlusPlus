#include "../tone_curve.hlsli"
#include "../dither.hlsli"

// The display mapping (src/display_output.cpp): the frame's linear light, 1 = paper white, into what a target shows. The
// 3D view arrives already fitted to the display's peak by the tone curve and colour corrected (shaders/amd/lens.hlsl,
// FinalMain), the HUD at paper white; light still above the peak is clipped. SDR targets take the sRGB curve, the game's
// gamma ramp (mat_monitorgamma) and dither; HDR10 targets saturation and the black level, then Rec. 2020 primaries and
// the PQ curve. The menu (an sRGB texture holding premultiplied colour) is composited over either.

#define DISPLAY_ROOT_SIGNATURE "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED), CBV(b0), " \
    "StaticSampler(s0, filter = FILTER_MIN_MAG_MIP_LINEAR, addressU = TEXTURE_ADDRESS_CLAMP, addressV = TEXTURE_ADDRESS_CLAMP, " \
    "addressW = TEXTURE_ADDRESS_CLAMP)"

static const uint kNoView = 0xFFFFFFFFu;
static const uint kEncodingSrgb = 0;
static const uint kEncodingPq = 1;
// The sRGB curve alone, clipped at white: a promoted render target read back as the 8-bit target it stands for.
static const uint kEncodingPlainSrgb = 2;

// Mirrors DisplayConstants in src/display_output.cpp, in 16-byte rows.
struct DisplayConstants
{
    // The frame's pixel at the target's corner, the frame, and the menu's texture or kNoView.
    int2 origin;
    uint frameView;
    uint uiView;
    uint encoding;
    // The display's peak in paper whites (1 in SDR), paper white in nits (PQ), the black level in nits (PQ).
    float peak;
    float paperWhite;
    float blackLevel;
    float saturation;
    // The menu's white in nits (PQ).
    float uiBrightness;
    // Dither steps across the encoded range (255 or 1023), 0 for none.
    float ditherSteps;
    // The game's gamma ramp in SDR (shaderapidx9.dll 0x180027c80): 0 none, 1 pow(x, gamma / 2.2), 2 with its TV range.
    uint gammaRamp;
    float gammaExponent;
    float tvExponent;
    float tvMin;
    float tvMax;
    // 16: the frame's luminance in nits (at paperWhite) in false colour; 17: its pixels at the display's peak (clipped there)
    // in magenta.
    uint debugView;
    float displayPeak;
    // 1 while the menu calibrates (display::SetCalibration): CalibrationPattern over the frame.
    uint calibration;
    uint padding;
};

ConstantBuffer<DisplayConstants> g_display : register(b0);
SamplerState g_linear : register(s0);

float3 SrgbToLinear(float3 color)
{
    return select(color <= 0.04045, color / 12.92, pow((max(color, 0.0) + 0.055) / 1.055, 2.4));
}

float3 LinearToSrgb(float3 color)
{
    color = max(color, 0.0);
    return select(color <= 0.0031308, color * 12.92, 1.055 * pow(color, 1.0 / 2.4) - 0.055);
}

float Luminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

// SMPTE ST 2084's inverse EOTF: absolute light in nits to the PQ signal.
float3 Pq(float3 nits)
{
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 4096.0 * 128.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 4096.0 * 32.0;
    const float c3 = 2392.0 / 4096.0 * 32.0;
    float3 y = pow(saturate(nits / 10000.0), m1);
    return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

struct VertexOutput
{
    float4 position : SV_Position;
};

[RootSignature(DISPLAY_ROOT_SIGNATURE)]
VertexOutput VSMain(uint vertex : SV_VertexID)
{
    float2 uv = float2((vertex << 1) & 2, vertex & 2);
    VertexOutput output;
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

// Luminance in nits as a false colour: blue at 1, green at 100, red from 1000 up, white at 10000.
float3 FalseColor(float nits)
{
    float t = saturate(log10(max(nits, 1e-4)) / 4.0);
    float3 ramp = t < 0.5 ? lerp(float3(0.0, 0.0, 1.0), float3(0.0, 1.0, 0.0), t * 2.0)
        : t < 0.75 ? lerp(float3(0.0, 1.0, 0.0), float3(1.0, 0.0, 0.0), (t - 0.5) * 4.0)
        : lerp(float3(1.0, 0.0, 0.0), float3(1.0, 1.0, 1.0), (t - 0.75) * 4.0);
    return ramp;
}

// The calibration pattern over the top three quarters of the screen, in paper whites (the game's frame shows below it,
// with the HUD at paper white): a patch at the peak with a square 1.1 times as bright inside, which disappears once the
// peak is set at or above what the display shows; paper white beside middle grey; near-black steps of 0, 0.5, 1, 2 and
// 5 nits. `inside` is false where the frame shows.
float3 CalibrationPattern(float2 pixel, float2 size, out bool inside)
{
    float2 uv = pixel / size;
    inside = uv.y < 0.75;
    if (!inside)
        return 0.0;
    float aspect = size.x / size.y;
    float2 peakOffset = abs(uv - float2(0.25, 0.3)) * float2(aspect, 1.0);
    float peakDistance = max(peakOffset.x, peakOffset.y);
    if (peakDistance < 0.05)
        return g_display.peak * 1.1;
    if (peakDistance < 0.18)
        return g_display.peak;
    if (uv.y > 0.15 && uv.y < 0.45) {
        if (uv.x > 0.5 && uv.x < 0.68)
            return 1.0;
        if (uv.x > 0.72 && uv.x < 0.9)
            return 0.18;
    }
    if (uv.y > 0.55 && uv.y < 0.7 && uv.x > 0.1 && uv.x < 0.9) {
        static const float kSteps[5] = {0.0, 0.5, 1.0, 2.0, 5.0};
        uint step = min(uint((uv.x - 0.1) / 0.16), 4u);
        return kSteps[step] / max(g_display.paperWhite, 1.0);
    }
    return 0.0;
}

[RootSignature(DISPLAY_ROOT_SIGNATURE)]
float4 PSMain(VertexOutput input) : SV_Target
{
    int2 pixel = int2(input.position.xy);
    Texture2D<float4> frame = ResourceDescriptorHeap[g_display.frameView];
    float3 color = max(frame.Load(int3(pixel + g_display.origin, 0)).rgb, 0.0);
    if (g_display.encoding == kEncodingPlainSrgb)
        return float4(LinearToSrgb(saturate(color)), 1.0);

    uint width, height;
    frame.GetDimensions(width, height);
    bool pattern = false;
    if (g_display.calibration != 0) {
        float3 calibrated = CalibrationPattern(float2(pixel + g_display.origin) + 0.5, float2(width, height), pattern);
        if (pattern)
            color = calibrated;
    }
    float3 mapped = min(color, g_display.peak * (pattern ? 1.1 : 1.0));
    bool clipped = max(color.r, max(color.g, color.b)) >= g_display.peak * 0.999;
    if (!pattern)
        mapped = max(lerp(Luminance(mapped), mapped, g_display.saturation), 0.0);

    float4 ui = 0.0;
    if (g_display.uiView != kNoView) {
        Texture2D<float4> menu = ResourceDescriptorHeap[g_display.uiView];
        ui = menu.Load(int3(pixel, 0));
    }

    if (g_display.encoding == kEncodingPq) {
        float3 nits = mapped * g_display.paperWhite;
        if (g_display.debugView == 16)
            nits = FalseColor(Luminance(color) * g_display.paperWhite) * g_display.paperWhite;
        else if (g_display.debugView == 17 && clipped)
            nits = float3(1.0, 0.0, 1.0) * g_display.paperWhite;
        // The black level lifts the darkest light, leaving the peak where it is.
        float peakNits = max(g_display.peak * g_display.paperWhite, 1.0);
        nits += g_display.blackLevel * saturate(1.0 - nits / peakNits);
        if (ui.a > 0.0) {
            float3 menu = SrgbToLinear(ui.rgb / ui.a) * g_display.uiBrightness;
            nits = menu * ui.a + nits * (1.0 - ui.a);
        }
        float3 signal = Pq(mul(kSrgbToRec2020, max(nits, 0.0)));
        if (g_display.ditherSteps > 0.0)
            signal += DitherNoise(uint2(pixel)) / g_display.ditherSteps;
        return float4(signal, 1.0);
    }

    if (g_display.debugView == 16)
        mapped = FalseColor(Luminance(color) * g_display.paperWhite);
    else if (g_display.debugView == 17 && clipped)
        mapped = float3(1.0, 0.0, 1.0);
    float3 encoded = LinearToSrgb(saturate(mapped));
    if (g_display.gammaRamp != 0) {
        encoded = pow(encoded, g_display.gammaExponent);
        if (g_display.gammaRamp == 2)
            encoded = saturate(pow(encoded, g_display.tvExponent) * (g_display.tvMax - g_display.tvMin) + g_display.tvMin);
    }
    encoded = ui.rgb + encoded * (1.0 - ui.a);
    if (g_display.ditherSteps > 0.0)
        encoded += DitherNoise(uint2(pixel)) / g_display.ditherSteps;
    return float4(encoded, 1.0);
}
