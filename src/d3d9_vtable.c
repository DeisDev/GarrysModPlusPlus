#define CINTERFACE
#include <stddef.h>
#include <d3d9.h>

#include "d3d9_vtable.h"

#define SLOT(Interface, Method) ((int)(offsetof(Interface##Vtbl, Method) / sizeof(void*)))

const int kSlotD3D9CreateDevice = SLOT(IDirect3D9, CreateDevice);
const int kSlotD3D9ExCreateDeviceEx = SLOT(IDirect3D9Ex, CreateDeviceEx);
const int kSlotDeviceReset = SLOT(IDirect3DDevice9, Reset);
const int kSlotDevicePresent = SLOT(IDirect3DDevice9, Present);
const int kSlotDeviceSetVertexShaderConstantF = SLOT(IDirect3DDevice9, SetVertexShaderConstantF);
const int kSlotDeviceCreateDepthStencilSurface = SLOT(IDirect3DDevice9, CreateDepthStencilSurface);
const int kSlotDeviceExPresentEx = SLOT(IDirect3DDevice9Ex, PresentEx);
const int kSlotDeviceExResetEx = SLOT(IDirect3DDevice9Ex, ResetEx);
