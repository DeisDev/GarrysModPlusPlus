#ifndef GMODPP_ATMOSPHERE_HLSLI
#define GMODPP_ATMOSPHERE_HLSLI

// Earth's atmosphere for the procedural sky (shaders/sky_atmosphere.hlsl, shaders/sky_draw.hlsl): Hillaire, "A Scalable
// and Production Ready Sky and Atmosphere Rendering Technique" (EGSR 2020), with the parameters of its reference
// implementation (SetupEarthAtmosphere, github.com/sebh/UnrealEngineSkyAtmosphere). Lengths are in kilometres and
// coefficients per kilometre; the planet's centre is the origin and +z is Source's up.
static const float kGroundRadius = 6360.0;
static const float kAtmosphereRadius = 6460.0;
static const float3 kRayleighScattering = float3(5.802e-3, 13.558e-3, 33.1e-3);
static const float kRayleighScaleHeight = 8.0;
static const float kMieScattering = 3.996e-3;
static const float kMieExtinction = 4.44e-3;
static const float kMieScaleHeight = 1.2;
static const float kMieAsymmetry = 0.8;
// Ozone absorbs between 10 and 40 km, most at 25 km.
static const float3 kOzoneAbsorption = float3(0.650e-3, 1.881e-3, 0.085e-3);
// Earth's mean albedo; the ground below the horizon takes the game's sky colours anyway (shaders/sky_atmosphere.hlsl).
static const float kGroundAlbedo = 0.3;
// The eye's height above the ground: a Source map spans a few kilometres at most.
static const float kEyeHeight = 0.2;

static const uint2 kTransmittanceSize = uint2(256, 64);
static const uint kMultipleScatteringSize = 32;

struct Medium
{
    float3 rayleigh;
    float mie;
    float3 extinction;
};

Medium MediumAt(float height)
{
    Medium medium;
    medium.rayleigh = kRayleighScattering * exp(-height / kRayleighScaleHeight);
    float mieDensity = exp(-height / kMieScaleHeight);
    medium.mie = kMieScattering * mieDensity;
    float ozone = saturate(1.0 - abs(height - 25.0) / 15.0);
    medium.extinction = medium.rayleigh + kMieExtinction * mieDensity + kOzoneAbsorption * ozone;
    return medium;
}

float RayleighPhase(float cosine)
{
    return 3.0 / (16.0 * 3.14159265) * (1.0 + cosine * cosine);
}

// Cornette-Shanks, as Hillaire's implementation uses for aerosols.
float MiePhase(float cosine)
{
    const float g = kMieAsymmetry;
    float k = 3.0 / (8.0 * 3.14159265) * (1.0 - g * g) / (2.0 + g * g);
    return k * (1.0 + cosine * cosine) / pow(max(1.0 + g * g - 2.0 * g * cosine, 1e-6), 1.5);
}

// The distance along a unit direction from `origin` to the sphere of `radius` around the planet's centre: the nearest
// crossing ahead, or -1 without one.
float SphereDistance(float3 origin, float3 direction, float radius)
{
    float distance = length(origin);
    float b = dot(origin, direction);
    float discriminant = b * b - (distance - radius) * (distance + radius);
    if (discriminant < 0.0)
        return -1.0;
    float root = sqrt(discriminant);
    float nearest = -b - root;
    float farthest = -b + root;
    return nearest >= 0.0 ? nearest : farthest >= 0.0 ? farthest : -1.0;
}

// A unit-range value at the centres of a table's first and last texels, and back (Bruneton, "Precomputed Atmospheric
// Scattering: a New Implementation", 2017).
float TextureCoordinate(float x, float size)
{
    return 0.5 / size + x * (1.0 - 1.0 / size);
}

float UnitRange(float u, float size)
{
    return (u - 0.5 / size) / (1.0 - 1.0 / size);
}

// The transmittance table's coordinates for a ray from radius r with cosine mu to the zenith, which does not meet the
// ground (Bruneton 2017's parametrisation: x by the distance to the top of the atmosphere, y by the horizon distance).
float2 TransmittanceUv(float r, float mu)
{
    float horizon = sqrt(kAtmosphereRadius * kAtmosphereRadius - kGroundRadius * kGroundRadius);
    float rho = sqrt(max(r * r - kGroundRadius * kGroundRadius, 0.0));
    float toTop = max(-r * mu + sqrt(max(r * r * (mu * mu - 1.0) + kAtmosphereRadius * kAtmosphereRadius, 0.0)), 0.0);
    float nearest = kAtmosphereRadius - r;
    float farthest = rho + horizon;
    return float2(TextureCoordinate((toTop - nearest) / (farthest - nearest), kTransmittanceSize.x),
        TextureCoordinate(rho / horizon, kTransmittanceSize.y));
}

void TransmittanceParameters(float2 uv, out float r, out float mu)
{
    float horizon = sqrt(kAtmosphereRadius * kAtmosphereRadius - kGroundRadius * kGroundRadius);
    float rho = horizon * UnitRange(uv.y, kTransmittanceSize.y);
    r = sqrt(rho * rho + kGroundRadius * kGroundRadius);
    float nearest = kAtmosphereRadius - r;
    float farthest = rho + horizon;
    float toTop = nearest + UnitRange(uv.x, kTransmittanceSize.x) * (farthest - nearest);
    mu = toTop == 0.0 ? 1.0 : clamp((horizon * horizon - rho * rho - toTop * toTop) / (2.0 * r * toTop), -1.0, 1.0);
}

float3 AtmosphereTransmittance(Texture2D<float4> table, SamplerState linearClamp, float r, float mu)
{
    return table.SampleLevel(linearClamp, TransmittanceUv(r, mu), 0.0).rgb;
}

// The multiple scattering table (Hillaire 2020, section 5.5) by the sun's cosine to the zenith and the height.
float2 MultipleScatteringUv(float r, float sunMu)
{
    float height = saturate((r - kGroundRadius) / (kAtmosphereRadius - kGroundRadius));
    return float2(TextureCoordinate(sunMu * 0.5 + 0.5, kMultipleScatteringSize), TextureCoordinate(height, kMultipleScatteringSize));
}

#endif
