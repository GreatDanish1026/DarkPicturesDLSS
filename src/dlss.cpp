// DLSS Super Resolution in place of The Quarry's TAAU dispatch (Direct3D 12).
//
// Per frame, on the game's command list, at the moment the game would dispatch its
// TAAU compute shader (inputs already in SRV state, output in UAV state):
//   1. dlss_prepare.hlsl (through ReShade) writes DLSS motion vectors and R32F depth
//      for the render view rect into two textures of ours;
//   2. NGX evaluates DLSS straight into the TAAU's own output texture.
// The caller then restores the game's compute state and skips the game's dispatch.
// NGX binds its own heaps, root signature and pipeline natively, so the caller has
// to re-bind the game's state through ReShade (see RestoreComputeState in probe.cpp).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>

#include "dlss.hpp"

#include <nvsdk_ngx_helpers_d3d.h>

#include <cstdio>
#include <unordered_map>

#include "dlss_prepare_cs.h"  // kDlssPrepareCS, compiled by build.ps1 from shaders/dlss_prepare.hlsl

#pragma comment(lib, "nvsdk_ngx_s.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "user32.lib")

namespace dlss {

Options g_options;
void (*g_log)(const char* fmt, ...) = nullptr;

namespace {

#define LOG(...)                       \
    do {                               \
        if (g_log) g_log(__VA_ARGS__); \
    } while (0)

// NGX project id for this mod (any fixed GUID; identifies the app to NGX).
constexpr char kProjectId[] = "6c2f3b1e-9d4a-4e57-8b6f-2a1d7c9e0f53";
constexpr uint32_t kConstants = 20;  // float4x4 + float2 + float2

enum class Init { untried, ok, failed };
Init g_init = Init::untried;
device* g_device = nullptr;
ID3D12Device* g_d3d = nullptr;
NVSDK_NGX_Parameter* g_params = nullptr;

NVSDK_NGX_Handle* g_feature = nullptr;
uint32_t g_featW = 0, g_featH = 0, g_featOutW = 0, g_featOutH = 0;
bool g_featJittered = false;

pipeline_layout g_layout = {};
pipeline g_pso = {};
bool g_pipelineFailed = false;

resource g_motion = {}, g_depth = {};
resource_view g_motionUav = {}, g_depthUav = {};
uint32_t g_texW = 0, g_texH = 0;

std::unordered_map<uint64_t, resource_view> g_srvs;  // game texture -> our SRV of it

std::string g_status = "waiting for the game's TAAU pass";
uint64_t g_done = 0, g_failures = 0;

void SetStatus(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_status != buf) LOG("dlss: %s", buf);
    g_status = buf;
}

bool InitNgx(device* dev) {
    g_device = dev;
    g_d3d = reinterpret_cast<ID3D12Device*>(dev->get_native());
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path, n);
    dir.resize(dir.find_last_of(L"\\/") + 1);

    NVSDK_NGX_Result r =
        NVSDK_NGX_D3D12_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_UNREAL, "4.26", dir.c_str(), g_d3d);
    if (NVSDK_NGX_FAILED(r)) {
        SetStatus("NGX init failed (0x%08x); is nvngx_dlss.dll next to the game exe?", static_cast<unsigned>(r));
        return false;
    }
    r = NVSDK_NGX_D3D12_GetCapabilityParameters(&g_params);
    if (NVSDK_NGX_FAILED(r) || !g_params) {
        SetStatus("NGX capability query failed (0x%08x)", static_cast<unsigned>(r));
        return false;
    }
    int available = 0, needsDriver = 0;
    NVSDK_NGX_Parameter_GetI(g_params, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    NVSDK_NGX_Parameter_GetI(g_params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriver);
    if (!available) {
        SetStatus("DLSS Super Resolution not available%s", needsDriver ? " (driver update needed)" : "");
        return false;
    }
    LOG("dlss: NGX initialised, DLSS Super Resolution available");
    return true;
}

bool CreatePipeline(device* dev) {
    pipeline_layout_param params[3];
    params[0] = pipeline_layout_param(constant_range{0, 0, 0, kConstants, shader_stage::compute});

    descriptor_range srvs;
    srvs.binding = 0;
    srvs.dx_register_index = 0;
    srvs.count = 2;
    srvs.visibility = shader_stage::compute;
    srvs.type = descriptor_type::shader_resource_view;
    descriptor_range uavs = srvs;
    uavs.type = descriptor_type::unordered_access_view;
    params[1].type = pipeline_layout_param_type::push_descriptors_with_ranges;
    params[1].descriptor_table = {1, &srvs};
    params[2].type = pipeline_layout_param_type::push_descriptors_with_ranges;
    params[2].descriptor_table = {1, &uavs};

    if (!dev->create_pipeline_layout(3, params, &g_layout)) {
        SetStatus("could not create the prepare pass root signature");
        return false;
    }
    shader_desc cs;
    cs.code = kDlssPrepareCS;
    cs.code_size = sizeof(kDlssPrepareCS);
    pipeline_subobject sub = {pipeline_subobject_type::compute_shader, 1, &cs};
    if (!dev->create_pipeline(g_layout, 1, &sub, &g_pso)) {
        SetStatus("could not create the prepare pass pipeline");
        return false;
    }
    return true;
}

void DestroyTextures(device* dev) {
    if (g_motionUav.handle) dev->destroy_resource_view(g_motionUav);
    if (g_depthUav.handle) dev->destroy_resource_view(g_depthUav);
    if (g_motion.handle) dev->destroy_resource(g_motion);
    if (g_depth.handle) dev->destroy_resource(g_depth);
    g_motionUav = g_depthUav = {};
    g_motion = g_depth = {};
    g_texW = g_texH = 0;
}

bool CreateTexture(device* dev, uint32_t w, uint32_t h, format fmt, resource& res, resource_view& uav) {
    const resource_desc desc(w, h, 1, 1, fmt, 1, memory_heap::default_,
                             resource_usage::shader_resource | resource_usage::unordered_access);
    return dev->create_resource(desc, nullptr, resource_usage::unordered_access, &res) &&
           dev->create_resource_view(res, resource_usage::unordered_access, resource_view_desc(fmt), &uav);
}

bool EnsureTextures(device* dev, uint32_t w, uint32_t h) {
    if (g_motion.handle && g_texW == w && g_texH == h) return true;
    DestroyTextures(dev);
    if (!CreateTexture(dev, w, h, format::r16g16_float, g_motion, g_motionUav) ||
        !CreateTexture(dev, w, h, format::r32_float, g_depth, g_depthUav)) {
        DestroyTextures(dev);
        SetStatus("could not create the %ux%u motion vector / depth textures", w, h);
        return false;
    }
    g_texW = w;
    g_texH = h;
    LOG("dlss: motion vector and depth textures %ux%u", w, h);
    return true;
}

resource_view GameSrv(device* dev, resource res, format fmt) {
    auto it = g_srvs.find(res.handle);
    if (it != g_srvs.end()) return it->second;
    resource_view view = {};
    if (!dev->create_resource_view(res, resource_usage::shader_resource, resource_view_desc(fmt), &view)) return {};
    g_srvs[res.handle] = view;
    return view;
}

NVSDK_NGX_PerfQuality_Value QualityFor(float fraction) {
    if (fraction > 0.9f) return NVSDK_NGX_PerfQuality_Value_DLAA;
    if (fraction > 0.62f) return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    if (fraction > 0.55f) return NVSDK_NGX_PerfQuality_Value_Balanced;
    if (fraction > 0.4f) return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
}

bool EnsureFeature(ID3D12GraphicsCommandList* list, const Frame& f) {
    if (g_feature && g_featW == f.renderW && g_featH == f.renderH && g_featOutW == f.outW && g_featOutH == f.outH &&
        g_featJittered == g_options.mvJittered)
        return true;
    if (g_feature) {
        // Sizes rarely change (window resize); the old feature may still be in flight, but NGX
        // defers the actual release until its GPU work is done.
        NVSDK_NGX_D3D12_ReleaseFeature(g_feature);
        g_feature = nullptr;
    }
    NVSDK_NGX_DLSS_Create_Params cp = {};
    cp.Feature.InWidth = f.renderW;
    cp.Feature.InHeight = f.renderH;
    cp.Feature.InTargetWidth = f.outW;
    cp.Feature.InTargetHeight = f.outH;
    cp.Feature.InPerfQualityValue = QualityFor(float(f.renderW) / float(f.outW));
    cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                              NVSDK_NGX_DLSS_Feature_Flags_DepthInverted | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure |
                              (g_options.mvJittered ? NVSDK_NGX_DLSS_Feature_Flags_MVJittered : 0);
    const NVSDK_NGX_Result r = NGX_D3D12_CREATE_DLSS_EXT(list, 1, 1, &g_feature, g_params, &cp);
    if (NVSDK_NGX_FAILED(r) || !g_feature) {
        g_feature = nullptr;
        SetStatus("DLSS feature creation failed (0x%08x) for %ux%u -> %ux%u", static_cast<unsigned>(r), f.renderW,
                  f.renderH, f.outW, f.outH);
        return false;
    }
    g_featW = f.renderW;
    g_featH = f.renderH;
    g_featOutW = f.outW;
    g_featOutH = f.outH;
    g_featJittered = g_options.mvJittered;
    LOG("dlss: feature created %ux%u -> %ux%u (quality mode %d)", f.renderW, f.renderH, f.outW, f.outH,
        static_cast<int>(cp.Feature.InPerfQualityValue));
    return true;
}

}  // namespace

Result Evaluate(const Frame& f) {
    if (!g_options.enabled) return Result::skipped;
    device* dev = f.cmd->get_device();
    if (dev->get_api() != device_api::d3d12) return Result::skipped;
    if (g_init == Init::untried) g_init = InitNgx(dev) ? Init::ok : Init::failed;
    if (g_init != Init::ok || dev != g_device) return Result::skipped;
    if (!g_pso.handle) {
        if (g_pipelineFailed) return Result::skipped;
        if (!CreatePipeline(dev)) {
            g_pipelineFailed = true;
            return Result::skipped;
        }
    }

    // Our textures match the game's render-target size; only the render rect is used.
    const resource_desc velocityDesc = dev->get_resource_desc(f.velocity);
    if (!EnsureTextures(dev, velocityDesc.texture.width, velocityDesc.texture.height)) return Result::skipped;
    const resource_view velocitySrv = GameSrv(dev, f.velocity, format::r16g16b16a16_unorm);
    const resource_view depthSrv = GameSrv(dev, f.depth, format::r32_float_x8_uint);
    if (!velocitySrv.handle || !depthSrv.handle) {
        SetStatus("could not create views of the game's velocity / depth");
        return Result::skipped;
    }

    // 1. Motion vectors and depth for DLSS.
    command_list* cmd = f.cmd;
    float constants[kConstants];
    memcpy(constants, f.clipToPrevClip, sizeof(float) * 16);
    constants[16] = float(f.renderW);
    constants[17] = float(f.renderH);
    constants[18] = 1.0f / float(f.renderW);
    constants[19] = 1.0f / float(f.renderH);
    cmd->bind_pipeline(pipeline_stage::compute_shader, g_pso);
    cmd->push_constants(shader_stage::compute, g_layout, 0, 0, kConstants, constants);
    const resource_view srvs[2] = {velocitySrv, depthSrv};
    const resource_view uavs[2] = {g_motionUav, g_depthUav};
    cmd->push_descriptors(shader_stage::compute, g_layout, 1,
                          descriptor_table_update{{}, 0, 0, 2, descriptor_type::shader_resource_view, srvs});
    cmd->push_descriptors(shader_stage::compute, g_layout, 2,
                          descriptor_table_update{{}, 0, 0, 2, descriptor_type::unordered_access_view, uavs});
    cmd->dispatch((f.renderW + 7) / 8, (f.renderH + 7) / 8, 1);

    const resource ours[2] = {g_motion, g_depth};
    const resource_usage uav[2] = {resource_usage::unordered_access, resource_usage::unordered_access};
    const resource_usage srv[2] = {resource_usage::shader_resource, resource_usage::shader_resource};
    cmd->barrier(2, ours, uav, srv);

    // 2. DLSS into the game's TAAU output.
    auto list = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd->get_native());
    Result result = Result::failed;
    if (EnsureFeature(list, f)) {
        NVSDK_NGX_D3D12_DLSS_Eval_Params e = {};
        e.Feature.pInColor = reinterpret_cast<ID3D12Resource*>(f.color.handle);
        e.Feature.pInOutput = reinterpret_cast<ID3D12Resource*>(f.output.handle);
        e.pInDepth = reinterpret_cast<ID3D12Resource*>(g_depth.handle);
        e.pInMotionVectors = reinterpret_cast<ID3D12Resource*>(g_motion.handle);
        e.InJitterOffsetX = g_options.flipJitterX ? -f.jitterX : f.jitterX;
        e.InJitterOffsetY = g_options.flipJitterY ? -f.jitterY : f.jitterY;
        e.InRenderSubrectDimensions = {f.renderW, f.renderH};
        e.InReset = f.reset ? 1 : 0;
        e.InMVScaleX = 1.0f;
        e.InMVScaleY = 1.0f;
        e.InOutputSubrectBase = {f.outX, f.outY};
        const NVSDK_NGX_Result r = NGX_D3D12_EVALUATE_DLSS_EXT(list, g_feature, g_params, &e);
        if (NVSDK_NGX_FAILED(r)) {
            ++g_failures;
            SetStatus("DLSS evaluate failed (0x%08x)", static_cast<unsigned>(r));
        } else {
            ++g_done;
            SetStatus("running: %ux%u -> %ux%u", f.renderW, f.renderH, f.outW, f.outH);
            result = Result::done;
        }
    }
    cmd->barrier(2, ours, srv, uav);
    return result;
}

void OnDestroyResource(resource r) {
    auto it = g_srvs.find(r.handle);
    if (it == g_srvs.end()) return;
    if (g_device) g_device->destroy_resource_view(it->second);
    g_srvs.erase(it);
}

void Shutdown(device* dev) {
    if (dev != g_device || !g_device) return;
    if (g_feature) NVSDK_NGX_D3D12_ReleaseFeature(g_feature);
    g_feature = nullptr;
    if (g_params) NVSDK_NGX_D3D12_DestroyParameters(g_params);
    g_params = nullptr;
    for (auto& [res, view] : g_srvs) dev->destroy_resource_view(view);
    g_srvs.clear();
    DestroyTextures(dev);
    if (g_pso.handle) dev->destroy_pipeline(g_pso);
    if (g_layout.handle) dev->destroy_pipeline_layout(g_layout);
    g_pso = {};
    g_layout = {};
    if (g_init == Init::ok) NVSDK_NGX_D3D12_Shutdown1(g_d3d);
    g_init = Init::untried;
    g_device = nullptr;
    g_d3d = nullptr;
    LOG("dlss: shut down");
}

std::string Status() {
    char buf[320];
    snprintf(buf, sizeof(buf), "%s (%llu frames, %llu failures)", g_status.c_str(),
             static_cast<unsigned long long>(g_done), static_cast<unsigned long long>(g_failures));
    return buf;
}

uint64_t FramesDone() { return g_done; }

}  // namespace dlss
