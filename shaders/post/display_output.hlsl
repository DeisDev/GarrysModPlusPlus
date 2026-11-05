#include "../tone_curve.hlsli"
#include "../dither.hlsli"

// The display mapping (src/display_output.cpp): the frame's linear light, 1 = the HUD's white (UiWhite in HDR, paper
// white in SDR), into what a target shows. The 3D view arrives fitted to the display's peak by the tone curve and colour
// corrected (shaders/amd/lens.hlsl, FinalMain); light still above the peak is clipped. SDR targets take the sRGB curve,
// the game's gamma ramp (mat_monitorgamma) and dither; HDR10 targets saturation and the black level, then Rec. 2020
// primaries and the PQ curve. The menu (an sRGB texture holding premultiplied colour) is composited over either in
// linear light, as the game's HUD blends.

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
    // PQ: paper white and the black level in nits.
    float paperWhite;
    float blackLevel;
    float saturation;
    // PQ: the HUD's and menu's white in nits, 0 for paper white (UiWhite).
    float uiWhite;
    // Dither steps across the encoded range (255 or 1023), 0 for none.
    float ditherSteps;
    // The game's gamma ramp in SDR (shaderapidx9.dll 0x180027c80): 0 none, 1 pow(x, gamma / 2.2), 2 with its TV range.
    uint gammaRamp;
    float gammaExponent;
    float tvExponent;
    float tvMin;
    float tvMax;
    // 16: the frame's luminance in nits in false colour; 17: its pixels at the display's peak (clipped there) in magenta.
    // The display's peak in nits (PQ).
    uint debugView;
    float displayPeak;
    // UiAlphaMain: the view without the HUD, mapped to this target's format (frame generation's, src/post_effects.cpp),
    // and the target's code values over its range (255 or 1023).
    uint hudlessView;
    float codeSteps;
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

// What the target shows at `pixel`, before it rounds to its code values.
float4 Display(int2 pixel)
{
    Texture2D<float4> frame = ResourceDescriptorHeap[g_display.frameView];
    float4 texel = frame.Load(int3(pixel + g_display.origin, 0));
    float3 color = max(texel.rgb, 0.0);
    // A render target read back keeps its alpha, as its 8-bit bytes did (ARC9's icons are cut out by it).
    if (g_display.encoding == kEncodingPlainSrgb)
        return float4(LinearToSrgb(saturate(color)), saturate(texel.a));

    bool pq = g_display.encoding == kEncodingPq;
    // The frame's 1 in nits and the display's peak in its units.
    float white = 1.0;
    float peak = 1.0;
    if (pq) {
        white = max(UiWhite(g_display.paperWhite, g_display.uiWhite), 1.0);
        peak = g_display.displayPeak / white;
    }
    float3 mapped = min(color, peak);
    bool clipped = max(color.r, max(color.g, color.b)) >= peak * 0.999;
    mapped = max(lerp(Luminance(mapped), mapped, g_display.saturation), 0.0);

    // The menu's premultiplied sRGB colour, as linear light over the frame's white.
    float4 ui = 0.0;
    if (g_display.uiView != kNoView) {
        Texture2D<float4> menu = ResourceDescriptorHeap[g_display.uiView];
        ui = menu.Load(int3(pixel, 0));
        if (ui.a > 0.0)
            ui.rgb = SrgbToLinear(ui.rgb / ui.a) * ui.a;
    }

    if (pq) {
        float3 nits = mapped * white;
        if (g_display.debugView == 16)
            nits = FalseColor(Luminance(color) * white) * g_display.paperWhite;
        else if (g_display.debugView == 17 && clipped)
            nits = float3(1.0, 0.0, 1.0) * g_display.paperWhite;
        // The black level lifts the darkest light, leaving the peak where it is.
        float peakNits = max(g_display.displayPeak, 1.0);
        nits += g_display.blackLevel * saturate(1.0 - nits / peakNits);
        nits = ui.rgb * white + nits * (1.0 - ui.a);
        float3 signal = Pq(mul(kSrgbToRec2020, max(nits, 0.0)));
        if (g_display.ditherSteps > 0.0)
            signal += DitherNoise(uint2(pixel)) / g_display.ditherSteps;
        return float4(signal, 1.0);
    }

    if (g_display.debugView == 16)
        mapped = FalseColor(Luminance(color) * g_display.paperWhite);
    else if (g_display.debugView == 17 && clipped)
        mapped = float3(1.0, 0.0, 1.0);
    mapped = ui.rgb + saturate(mapped) * (1.0 - ui.a);
    float3 encoded = LinearToSrgb(mapped);
    if (g_display.gammaRamp != 0) {
        encoded = pow(encoded, g_display.gammaExponent);
        if (g_display.gammaRamp == 2)
            encoded = saturate(pow(encoded, g_display.tvExponent) * (g_display.tvMax - g_display.tvMin) + g_display.tvMin);
    }
    if (g_display.ditherSteps > 0.0)
        encoded += DitherNoise(uint2(pixel)) / g_display.ditherSteps;
    return float4(encoded, 1.0);
}

[RootSignature(DISPLAY_ROOT_SIGNATURE)]
float4 PSMain(VertexOutput input) : SV_Target
{
    return Display(int2(input.position.xy));
}

struct UiAlphaOutput
{
    float4 color : SV_Target0;
    float uiAlpha : SV_Target1;
};

// PSMain, and DLSS Frame Generation's UI alpha (Streamline's kBufferTypeUIAlpha): 1 where what the target shows differs
// from the view without the HUD by more than a code value (rounding to the target keeps the two within one elsewhere),
// which only what was drawn after it changes (the HUD, menus, the performance overlay), as both take the same mapping
// and dither. Without it, Frame Generation warped the HUD
// with the view's motion (NVIDIA's DLSS-G guide 5.1: a hudless and a UI buffer are critical for the HUD; 2026-11-04).
// Translucent HUD counts as opaque, so what lies behind it is shown as rendered, not generated.
[RootSignature(DISPLAY_ROOT_SIGNATURE)]
UiAlphaOutput UiAlphaMain(VertexOutput input)
{
    int2 pixel = int2(input.position.xy);
    UiAlphaOutput output;
    output.color = Display(pixel);
    Texture2D<float4> hudless = ResourceDescriptorHeap[g_display.hudlessView];
    float3 difference = abs(saturate(output.color.rgb) - hudless.Load(int3(pixel, 0)).rgb);
    output.uiAlpha = any(difference * g_display.codeSteps > 1.0) ? 1.0 : 0.0;
    return output;
}

