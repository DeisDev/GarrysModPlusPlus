#ifndef GMODPP_CUBE_HLSLI
#define GMODPP_CUBE_HLSLI

// The direction through a cube face at (u, v) in [-1, 1], v down, in Direct3D's face order and orientation.
float3 FaceDirection(uint face, float2 uv)
{
    switch (face) {
    case 0: return float3(1.0, -uv.y, -uv.x);
    case 1: return float3(-1.0, -uv.y, uv.x);
    case 2: return float3(uv.x, 1.0, uv.y);
    case 3: return float3(uv.x, -1.0, -uv.y);
    case 4: return float3(uv.x, -uv.y, 1.0);
    default: return float3(-uv.x, -uv.y, -1.0);
    }
}

// The face a direction passes through and where, (u, v) in [-1, 1]; the inverse of FaceDirection.
float2 CubeFaceUv(float3 direction, out uint face)
{
    float3 a = abs(direction);
    if (a.x >= a.y && a.x >= a.z) {
        face = direction.x > 0.0 ? 0 : 1;
        return float2(direction.x > 0.0 ? -direction.z : direction.z, -direction.y) / a.x;
    }
    if (a.y >= a.z) {
        face = direction.y > 0.0 ? 2 : 3;
        return float2(direction.x, direction.y > 0.0 ? direction.z : -direction.z) / a.y;
    }
    face = direction.z > 0.0 ? 4 : 5;
    return float2(direction.z > 0.0 ? direction.x : -direction.x, -direction.y) / a.z;
}

// The texel (x, y, face) of a size x size cube that a direction passes through.
uint3 CubeTexel(float3 direction, uint size)
{
    uint face;
    float2 uv = CubeFaceUv(direction, face);
    return uint3(min(uint2((uv * 0.5 + 0.5) * float(size)), size - 1), face);
}

#endif
