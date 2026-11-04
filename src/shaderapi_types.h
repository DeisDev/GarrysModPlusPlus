#pragma once

// Structures the shader API interfaces exchange with materialsystem, as declared in the Source SDK 2013 headers
// (public/shaderapi/IShaderDevice.h, public/materialsystem/imaterialsystem.h, public/appframework/IAppSystem.h).

namespace gmodpp::native {

enum InitReturnVal : int { kInitFailed = 0, kInitOk = 1 };

// ImageFormat values used by the backend (public/bitmap/imageformat.h).
enum ImageFormat : int { kImageFormatBgra8888 = 12, kImageFormatBgrx8888 = 16, kImageFormatRgba16161616F = 24 };

constexpr int kShaderDisplayModeVersion = 1;
constexpr int kMaterialAdapterNameLength = 512;

struct ShaderDisplayMode {
    int version;
    int width;
    int height;
    int format;
    int refreshRateNumerator;
    int refreshRateDenominator;
};

struct ShaderDeviceInfo {
    int version;
    ShaderDisplayMode displayMode;
    int backBufferCount;
    int aaSamples;
    int aaQuality;
    int dxLevel;
    int windowedSizeLimitWidth;
    int windowedSizeLimitHeight;
    bool windowed : 1;
    bool resizing : 1;
    bool useStencil : 1;
    bool limitWindowedSize : 1;
    bool waitForVSync : 1;
    bool scaleToOutputResolution : 1;
    bool progressive : 1;
    bool usingMultipleWindows : 1;
};

struct MaterialAdapterInfo {
    char driverName[kMaterialAdapterNameLength];
    unsigned int vendorId;
    unsigned int deviceId;
    unsigned int subSysId;
    unsigned int revision;
    int dxSupportLevel;
    int maxDxSupportLevel;
    unsigned int driverVersionHigh;
    unsigned int driverVersionLow;
};

}
