#include "../tone_curve.hlsli"
#include "../dither.hlsli"

// The display mapping (src/display_output.cpp): the frame's linear light, 1 = paper white, into what a target shows. SDR
// targets take the tone curve, the engine's colour correction, saturation, the sRGB curve, the game's gamma ramp
// (mat_monitorgamma) and dither; HDR10 targets the tone curve up to the display's peak, colour correction below paper
// white, saturation and the black level, then Rec. 2020 primaries and the PQ curve. The menu (an sRGB texture holding
// premultiplied colour) is composited over either.

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
    uint toneCurve;
    float highlightRolloff;
    // The curve's peak in paper whites (1 in SDR), paper white in nits (PQ), the black level in nits (PQ).
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
    // The engine's colour correction (Engine_Post_ps2x.fxc's PerformColorCorrection): its look-up count, default weight,
    // weights and 32^3 volumes.
    uint colorCorrection;
    float ccDefaultWeight;
    float4 ccWeights;
    uint4 ccViews;
    // 16: the frame's luminance in nits (at paperWhite) in false colour; 17: the frame's pixels brighter than the display's
    // peak (displayPeak, nits) in magenta.
    uint debugView;
    float displayPeak;
    // Calibration pattern (display::Calibration), 0 for none.
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

// The engine's colour correction over sRGB-encoded colour, as engine_post blends its look-ups.
float3 ColorCorrected(float3 encoded)
{
    float3 lookup = encoded * (31.0 / 32.0) + 0.5 / 32.0;
    float3 result = encoded * g_display.ccDefaultWeight;
    [loop]
    for (uint i = 0; i < g_display.colorCorrection; ++i) {
        Texture3D<float4> volume = ResourceDescriptorHeap[NonUniformResourceIndex(g_display.ccViews[i])];
        result += volume.SampleLevel(g_linear, lookup, 0.0).rgb * g_display.ccWeights[i];
    }
    return result;
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

// The calibration patterns, in paper whites, or a negative value outside them.
float3 CalibrationPattern(float2 pixel, float2 size, out bool inside)
{
    inside = true;
    float2 uv = pixel / size;
    float peak = g_display.peak;
    // A peak patch on the left: the slider's peak, and inside it a square 1.1 times as bright that vanishes once the
    // peak is set at or below the display's.
    if (g_display.calibration == 1) {
        float2 centre = float2(0.25, 0.5);
        float2 offset = abs(uv - centre) * float2(size.x / size.y, 1.0);
        if (max(offset.x, offset.y) < 0.06)
            return peak * 1.1;
        if (max(offset.x, offset.y) < 0.2)
            return peak;
        return 0.0;
    }
    // Paper white beside middle grey, and near-black steps of 0, 0.5, 1, 2 and 5 nits along the bottom.
    if (g_display.calibration == 2) {
        if (uv.y > 0.75) {
            uint step = min(uint(uv.x * 5.0), 4u);
            static const float kSteps[5] = {0.0, 0.5, 1.0, 2.0, 5.0};
            return kSteps[step] / max(g_display.paperWhite, 1.0);
        }
        if (uv.y > 0.3 && uv.y < 0.6) {
            if (uv.x > 0.2 && uv.x < 0.45)
                return 1.0;
            if (uv.x > 0.55 && uv.x < 0.8)
                return 0.18;
        }
        return 0.0;
    }
    inside = false;
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
    float3 mapped = pattern ? min(color, g_display.peak * 1.1) : ToneCurve(color, g_display.toneCurve, g_display.highlightRolloff, g_display.peak);
    if (g_display.colorCorrection != 0 && !pattern) {
        // Below paper white as the game corrected its 8-bit image; above it unchanged.
        float3 low = saturate(mapped);
        mapped = SrgbToLinear(ColorCorrected(LinearToSrgb(low))) + (mapped - low);
    }
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
        else if (g_display.debugView == 17 && max(color.r, max(color.g, color.b)) * g_display.paperWhite > g_display.displayPeak)
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
    else if (g_display.debugView == 17 && max(color.r, max(color.g, color.b)) * g_display.paperWhite > g_display.displayPeak)
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
