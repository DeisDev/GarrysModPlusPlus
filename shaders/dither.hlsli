#ifndef GMODPP_DITHER_HLSLI
#define GMODPP_DITHER_HLSLI

uint DitherHash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// Noise of a triangular distribution over (-1, 1) steps of an 8-bit target, added to a colour before it is rounded to
// the target: smooth gradients (the sky, fog) then dither instead of breaking into bands.
float DitherNoise(uint2 pixel)
{
    uint a = DitherHash(pixel.x + pixel.y * 65536u);
    uint b = DitherHash(a);
    return (float(a >> 8) + float(b >> 8)) * (1.0 / 16777216.0) - 1.0;
}

#endif
