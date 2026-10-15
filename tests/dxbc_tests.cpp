// Checks ParseDxbc against shaders compiled with the system D3DCompiler.
// Build and run: .\build.ps1 -Test

#include <windows.h>
#include <d3dcompiler.h>

#include <cstdio>

#include "../src/dxbc.hpp"

#pragma comment(lib, "d3dcompiler.lib")

static int g_failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            ++g_failures;                                              \
        }                                                              \
    } while (0)

static ShaderDecl Compile(const char* src, const char* target) {
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3DCompile(src, strlen(src), "test", nullptr, nullptr, "main", target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) {
        printf("compile failed: %s\n", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        ++g_failures;
        return {};
    }
    // Strip reflection like a shipping UE4 build, so only the declarations remain.
    ID3DBlob* stripped = nullptr;
    D3DStripShader(code->GetBufferPointer(), code->GetBufferSize(),
                   D3DCOMPILER_STRIP_REFLECTION_DATA | D3DCOMPILER_STRIP_DEBUG_INFO, &stripped);
    ID3DBlob* use = stripped ? stripped : code;
    ShaderDecl d = ParseDxbc(use->GetBufferPointer(), use->GetBufferSize());
    if (stripped) stripped->Release();
    code->Release();
    return d;
}

int main() {
    // TAA-like compute shader: textures, structured and raw buffers, typed and structured UAVs, two cbuffers.
    const ShaderDecl cs = Compile(R"(
        cbuffer A : register(b0) { float4 a[4]; };
        cbuffer B : register(b2) { float4 jitter; float4 more[9]; };
        Texture2D<float4> color : register(t1);
        Texture2D<float> depth : register(t2);
        Texture2D<float4> velocity : register(t3);
        StructuredBuffer<float4> sb : register(t7);
        ByteAddressBuffer raw : register(t65);
        RWTexture2D<float4> outColor : register(u0);
        RWStructuredBuffer<float> outSb : register(u3);
        [numthreads(8, 8, 1)] void main(uint2 id : SV_DispatchThreadID) {
            float4 c = color[id] + velocity[id] * depth[id] + sb[id.x] + asfloat(raw.Load(id.x * 4));
            outColor[id] = c * a[id.x & 3] + jitter + more[id.y % 9];
            outSb[id.x] = c.x;
        })", "cs_5_0");
    CHECK(cs.valid);
    for (uint32_t i = 0; i < 128; ++i)
        CHECK(cs.HasSrv(i) == (i == 1 || i == 2 || i == 3 || i == 7 || i == 65));
    for (uint32_t i = 0; i < 64; ++i) CHECK(cs.HasUav(i) == (i == 0 || i == 3));
    for (uint32_t i = 0; i < 14; ++i) CHECK(cs.HasCb(i) == (i == 0 || i == 2));
    CHECK(cs.cbVec4[0] == 4);
    CHECK(cs.cbVec4[2] == 10);

    // Full-screen pixel shader.
    const ShaderDecl ps = Compile(R"(
        cbuffer P : register(b1) { float4 tint; };
        Texture2D t : register(t5);
        SamplerState s : register(s0);
        float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target { return t.Sample(s, uv) * tint; })",
                                  "ps_5_0");
    CHECK(ps.valid);
    for (uint32_t i = 0; i < 128; ++i) CHECK(ps.HasSrv(i) == (i == 5));
    CHECK(ps.uav == 0);
    CHECK(ps.cb == (1u << 1));
    CHECK(ps.cbVec4[1] == 1);

    // Not DXBC.
    const char junk[64] = "not a shader";
    CHECK(!ParseDxbc(junk, sizeof(junk)).valid);
    CHECK(!ParseDxbc(nullptr, 0).valid);

    printf(g_failures ? "%d check(s) failed\n" : "all dxbc tests passed\n", g_failures);
    return g_failures ? 1 : 0;
}
