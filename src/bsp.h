#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace gmodpp::bsp {

struct Float3 {
    float x, y, z;
};

struct SunLight {
    bool present = false;
    Float3 direction = {};
    Float3 color = {};
    Float3 ambient = {};
};

struct StaticProp {
    uint32_t model;
    float transform[3][4];
};

struct WorldGeometry {
    std::vector<Float3> positions;
    std::vector<uint32_t> indices;
    SunLight sun;
    uint32_t faceCount = 0;
    uint32_t displacementCount = 0;
    uint32_t skyboxFaceCount = 0;
    std::vector<std::string> propModels;
    std::vector<StaticProp> props;
};

// Returns false with a reason in `error` when the data is not a supported Source BSP.
bool LoadWorld(std::span<const std::byte> file, WorldGeometry& world, std::string& error);

}
