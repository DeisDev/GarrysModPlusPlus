#pragma once

#include <d3d9.h>

namespace gmodpp::hooks {

void InstallInterfaceHooks(IDirect3D9* d3d);
void InstallInterfaceHooks(IDirect3D9Ex* d3d);

}
