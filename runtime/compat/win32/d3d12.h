#pragma once
#include <Windows.h>
#include <directx/d3d12.h>
// __uuidof bindings for every D3D12 interface (DirectX-Headers).
#include <dxguids/dxguids.h>
#ifndef GALAXY_ID3D10BLOB_UUID
#define GALAXY_ID3D10BLOB_UUID
// d3dcommon.h's blob is not covered by dxguids.h.
__CRT_UUID_DECL(ID3D10Blob, 0x8ba5fb08, 0x5195, 0x40e2, 0xac, 0x58, 0x0d, 0x98, 0x9c, 0x3a, 0x01, 0x02)
#endif
