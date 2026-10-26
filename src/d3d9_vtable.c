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
const int kSlotDeviceSetRenderTarget = SLOT(IDirect3DDevice9, SetRenderTarget);
const int kSlotDeviceSetDepthStencilSurface = SLOT(IDirect3DDevice9, SetDepthStencilSurface);
const int kSlotDeviceSetViewport = SLOT(IDirect3DDevice9, SetViewport);
const int kSlotDeviceClear = SLOT(IDirect3DDevice9, Clear);
const int kSlotDeviceBeginScene = SLOT(IDirect3DDevice9, BeginScene);
const int kSlotDeviceEndScene = SLOT(IDirect3DDevice9, EndScene);
const int kSlotDeviceDrawPrimitive = SLOT(IDirect3DDevice9, DrawPrimitive);
const int kSlotDeviceDrawIndexedPrimitive = SLOT(IDirect3DDevice9, DrawIndexedPrimitive);
const int kSlotDeviceDrawPrimitiveUP = SLOT(IDirect3DDevice9, DrawPrimitiveUP);
const int kSlotDeviceDrawIndexedPrimitiveUP = SLOT(IDirect3DDevice9, DrawIndexedPrimitiveUP);
const int kSlotDeviceStretchRect = SLOT(IDirect3DDevice9, StretchRect);
const int kSlotDeviceSetVertexShader = SLOT(IDirect3DDevice9, SetVertexShader);
const int kSlotDeviceSetPixelShader = SLOT(IDirect3DDevice9, SetPixelShader);
const int kSlotDeviceCreatePixelShader = SLOT(IDirect3DDevice9, CreatePixelShader);
const int kSlotDeviceSetPixelShaderConstantF = SLOT(IDirect3DDevice9, SetPixelShaderConstantF);
const int kSlotDeviceSetRenderState = SLOT(IDirect3DDevice9, SetRenderState);
const int kSlotDeviceCreateVertexShader = SLOT(IDirect3DDevice9, CreateVertexShader);
const int kSlotDeviceSetTexture = SLOT(IDirect3DDevice9, SetTexture);
const int kSlotDeviceExPresentEx = SLOT(IDirect3DDevice9Ex, PresentEx);
const int kSlotDeviceExResetEx = SLOT(IDirect3DDevice9Ex, ResetEx);
