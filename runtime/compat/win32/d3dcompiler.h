// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// D3DCompile for the Apple port. "Compiling" HLSL produces a blob holding
// SPIR-V (glslang, see galaxy/gx/shader_translate.h) and the shader's stage
// and entry point; the Metal backend translates it to MSL when a pipeline
// is created. Errors are reported here, as fxc reports them on Windows.

#include <Windows.h>
#include <directx/d3dcommon.h>
#include <d3d12shader.h>

struct ID3DInclude;
#define D3D_COMPILE_STANDARD_FILE_INCLUDE ((ID3DInclude*)(UINT_PTR)1)

#define D3DCOMPILE_DEBUG (1 << 0)
#define D3DCOMPILE_SKIP_VALIDATION (1 << 1)
#define D3DCOMPILE_SKIP_OPTIMIZATION (1 << 2)
#define D3DCOMPILE_PACK_MATRIX_ROW_MAJOR (1 << 3)
#define D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR (1 << 4)
#define D3DCOMPILE_PARTIAL_PRECISION (1 << 5)
#define D3DCOMPILE_AVOID_FLOW_CONTROL (1 << 9)
#define D3DCOMPILE_PREFER_FLOW_CONTROL (1 << 10)
#define D3DCOMPILE_ENABLE_STRICTNESS (1 << 11)
#define D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY (1 << 12)
#define D3DCOMPILE_IEEE_STRICTNESS (1 << 13)
#define D3DCOMPILE_OPTIMIZATION_LEVEL0 (1 << 14)
#define D3DCOMPILE_OPTIMIZATION_LEVEL1 0
#define D3DCOMPILE_OPTIMIZATION_LEVEL2 ((1 << 14) | (1 << 15))
#define D3DCOMPILE_OPTIMIZATION_LEVEL3 (1 << 15)
#define D3DCOMPILE_WARNINGS_ARE_ERRORS (1 << 18)
#define D3DCOMPILE_RESOURCES_MAY_ALIAS (1 << 19)
#define D3DCOMPILE_ALL_RESOURCES_BOUND (1 << 21)

HRESULT D3DCompile(
    const void* source, SIZE_T source_size, const char* source_name,
    const D3D_SHADER_MACRO* defines, ID3DInclude* include, const char* entry_point,
    const char* target, UINT flags1, UINT flags2, ID3DBlob** code, ID3DBlob** errors);

HRESULT D3DCreateBlob(SIZE_T size, ID3DBlob** blob);

// Reflection of a D3DCompile blob: the program type (D3D12_SHADER_DESC::
// Version) only.
HRESULT D3DReflect(const void* data, SIZE_T size, REFIID riid, void** reflector);
