// DLSS Frame Generation (DLSS-G) through NVIDIA Streamline.
//
// Streamline runs in manual hooking mode (sl.interposer.dll loaded at run time, never
// linked), because the game's D3D12 device and swap chain already come through ReShade.
// DLSS-G needs its proxy swap chain in the present path and expects to see that swap chain
// being created. So when frame generation is on at launch, the IDXGIFactory vtable (shared by
// every DXGI factory) is patched and the swap chain creation goes through an SL factory
// proxy. ReShade wraps the game's factory and calls the real one, so the call arrives from
// ReShade and Streamline ends up below it: game -> ReShade -> Streamline -> DXGI. ReShade
// then sees real frames only, and its overlay is interpolated with the game.
//
// Per frame: at the TAAU dispatch, after DLSS Super Resolution, TagFrame gives Streamline
// our motion vectors and depth (the same ones DLSS SR gets) and the camera constants from
// the View uniform buffer. At the game's Present: DLSS-G is paused for frames DLSS SR didn't
// tag (DLSS off, loading screens, camera-cut frames) and around swap chain resizes; PCL
// markers go around the Present and the Reflex sleep right after it, since there is no hook
// into the game's simulation loop.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <intrin.h>

#include "fg.hpp"

#include <sl.h>
#include <sl_helpers.h>
#include <sl_security.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace fg {

Options g_options;
void (*g_log)(const char* fmt, ...) = nullptr;

namespace {

#define LOG(...)                       \
    do {                               \
        if (g_log) g_log(__VA_ARGS__); \
    } while (0)

constexpr char kSection[] = "QuarryDLSS";
// Same id as the NGX project id in dlss.cpp.
constexpr char kProjectId[] = "6c2f3b1e-9d4a-4e57-8b6f-2a1d7c9e0f53";
// The Streamline runtime the user copies next to the game exe (build.ps1 -InstallStreamline).
const wchar_t* const kFiles[] = {L"sl.interposer.dll", L"sl.common.dll", L"sl.dlss_g.dll",
                                 L"sl.reflex.dll",     L"sl.pcl.dll",    L"nvngx_dlssg.dll"};
constexpr uint32_t kPauseLogFrames = 30;  // untagged frames in a row before a pause is logged / shown
constexpr uint32_t kResizeFrames = 10;    // presents DLSS-G stays off after a resize

const sl::ViewportHandle& Viewport() {
    static const sl::ViewportHandle v(0u);
    return v;
}

std::wstring GameDir() {
    wchar_t path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path, n);
    return dir.substr(0, dir.find_last_of(L"\\/") + 1);
}

HMODULE ModuleOf(const void* address) {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(address), &m);
    return m;
}

std::string ModuleName(HMODULE m) {
    if (!m) return "(unknown)";
    char path[MAX_PATH] = {};
    GetModuleFileNameA(m, path, MAX_PATH);
    const char* slash = strrchr(path, '\\');
    return slash ? slash + 1 : path;
}

bool FromGame(const void* ret) { return ModuleOf(ret) == GetModuleHandleW(nullptr); }
bool FromReShade(const void* ret) { return ModuleOf(ret) == reshade::internal::get_reshade_module_handle(); }

// ReShade and the system DXGI are both "dxgi.dll": say which.
std::string Caller(const void* ret) {
    if (FromReShade(ret)) return "ReShade";
    if (FromGame(ret)) return "the game exe";
    return ModuleName(ModuleOf(ret));
}

// Set while we call into the SL factory proxy, so its call back into the patched native
// function on this thread goes straight through.
thread_local bool t_inside = false;
struct Inside {
    Inside() { t_inside = true; }
    ~Inside() { t_inside = false; }
};

// ---- vtable patches, all undone by Shutdown -------------------------------------------

struct Patch {
    void** slot;
    void* original;
};
std::vector<Patch> g_patches;

template <class T>
bool PatchSlot(void* object, int index, T replacement, T& original, const char* name) {
    void** slot = *reinterpret_cast<void***>(object) + index;
    if (*slot == reinterpret_cast<void*>(replacement)) return true;  // already ours
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        LOG("fg: could not unprotect the %s vtable slot", name);
        return false;
    }
    original = reinterpret_cast<T>(*slot);
    *slot = reinterpret_cast<void*>(replacement);
    VirtualProtect(slot, sizeof(void*), old, &old);
    g_patches.push_back({slot, reinterpret_cast<void*>(original)});
    LOG("fg: patched %s (was %s+0x%llx)", name, ModuleName(ModuleOf(original)).c_str(),
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(original) -
                                        reinterpret_cast<uintptr_t>(ModuleOf(original))));
    return true;
}

void UnpatchAll() {
    for (auto it = g_patches.rbegin(); it != g_patches.rend(); ++it) {
        DWORD old = 0;
        if (!VirtualProtect(it->slot, sizeof(void*), PAGE_READWRITE, &old)) continue;
        *it->slot = it->original;
        VirtualProtect(it->slot, sizeof(void*), old, &old);
    }
    if (!g_patches.empty()) LOG("fg: restored %zu vtable entries", g_patches.size());
    g_patches.clear();
}

// ---- Streamline ------------------------------------------------------------------------

struct Sl {
    HMODULE module = nullptr;
    PFun_slInit* init = nullptr;
    PFun_slSetD3DDevice* setD3DDevice = nullptr;
    PFun_slUpgradeInterface* upgrade = nullptr;
    PFun_slGetNewFrameToken* newFrameToken = nullptr;
    PFun_slSetConstants* setConstants = nullptr;
    PFun_slSetTagForFrame* setTag = nullptr;
    PFun_slGetFeatureFunction* featureFunction = nullptr;
    PFun_slIsFeatureSupported* isSupported = nullptr;
    PFun_slIsFeatureLoaded* isLoaded = nullptr;
    PFun_slGetFeatureRequirements* requirements = nullptr;
    PFun_slGetFeatureVersion* version = nullptr;
    PFun_slFreeResources* freeResources = nullptr;
    PFun_slDLSSGSetOptions* dlssgSetOptions = nullptr;
    PFun_slDLSSGGetState* dlssgGetState = nullptr;
    PFun_slReflexSetOptions* reflexSetOptions = nullptr;
    PFun_slReflexSleep* reflexSleep = nullptr;
    PFun_slPCLSetMarker* pclSetMarker = nullptr;
};
Sl g_sl;

enum class State { untried, ok, failed };
State g_state = State::untried;
bool g_filesPresent = false;  // all of kFiles next to the exe (checked at load)
bool g_hooked = false;        // the factory hook is in place (frame generation was on at launch)
ID3D12Device* g_device = nullptr;  // native: the last D3D12 device created (the game's real one by swap chain time)
LUID g_luid = {};

std::mutex g_mutex;                   // the fields below that TagFrame and the present path share
sl::FrameToken* g_token = nullptr;    // the frame being built (tagged at the TAAU, presented next)
uint32_t g_taggedFrame = UINT32_MAX;  // frame index TagFrame last tagged
bool g_optionsDirty = true;
uint32_t g_resizeFrames = 0;          // presents left with DLSS-G off after a resize
uint32_t g_backBufferW = 0, g_backBufferH = 0;  // the SL swap chain's, read at creation and after each resize
// Set by a resize: DLSS-G's resources (sized for the old swap chain) are freed after the next
// present with it off, so it reallocates when it comes back on. Run 6: turning it back on with
// the kept 1280x720 resources after an F11 back to 3840x2160 hung the GPU.
std::atomic<bool> g_freePending{false};

// Present thread only.
Mode g_applied = Mode::count;         // what DLSS-G was last set to (count: nothing yet)
bool g_reflexApplied = false;
bool g_dynamicSupported = false;
uint32_t g_maxFrames = 1;             // DLSSGState::numFramesToGenerateMax
uint32_t g_dlssgStatus = 0xffffffffu;
uint32_t g_untagged = 0;              // untagged frames in a row
std::atomic<uint64_t> g_realPresents{0};
uint64_t g_countedPresents = 0;       // real presents the probe has counted
// The game's CreateSwapChain reached us through ReShade (ReShade wraps the game's factory
// and calls the real one): Streamline then sits below ReShade, which sees real frames only.
bool g_reshadeAbove = false;
uint32_t g_errors = 0;                // logged SL call failures, capped

std::mutex g_statusMutex;  // g_status / g_friendly: set on the game's threads, read by the overlay
std::string g_status = "off";
std::string g_running;     // "3x, 52 -> 156 fps" from the last window

void SetStatus(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard lock(g_statusMutex);
    if (g_status != buf) LOG("fg: %s", buf);
    g_status = buf;
}

bool Check(sl::Result r, const char* what) {
    if (r == sl::Result::eOk) return true;
    if (++g_errors <= 60) LOG("fg: %s failed: %s", what, sl::getResultAsStr(r));
    return false;
}

// Streamline's own log lines. Info only with Verbose log; a warning or error that repeats
// (one per frame, say) is logged 3 times, then every 1000th time.
void OnSlLog(sl::LogType type, const char* msg) {
    if (type == sl::LogType::eInfo && !g_options.verboseLog) return;
    std::string m = msg ? msg : "";
    while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
    const char* kind = type == sl::LogType::eError ? "error" : type == sl::LogType::eWarn ? "warn" : "info";
    if (type != sl::LogType::eInfo) {
        // "[time][streamline][warn][tid][elapsed]file.cpp:line[func] text": key on what follows the 5th ']'.
        size_t at = 0;
        for (int i = 0; i < 5 && at != std::string::npos; ++i) at = m.find(']', at ? at + 1 : 0);
        const std::string key = at == std::string::npos ? m : m.substr(at + 1);
        static std::mutex mutex;
        static std::unordered_map<std::string, uint32_t> seen;
        std::lock_guard lock(mutex);
        const uint32_t n = seen.size() < 500 ? ++seen[key] : 1;
        if (n > 3 && n % 1000 != 0) return;
        if (n > 3) {
            LOG("sl %s (seen %u times): %s", kind, n, m.c_str());
            return;
        }
    }
    LOG("sl %s: %s", kind, m.c_str());
}

template <class T>
bool GetFeatureFunction(sl::Feature feature, const char* name, T*& out) {
    void* p = nullptr;
    if (!Check(g_sl.featureFunction(feature, name, p), name) || !p) return false;
    out = reinterpret_cast<T*>(p);
    return true;
}

// Loads and initialises Streamline once, at swap chain time (only the game's real device
// gets a swap chain). Never retried after a failure.
bool EnsureSl() {
    if (g_state != State::untried) return g_state == State::ok;
    g_state = State::failed;
    if (!g_device) {
        SetStatus("no D3D12 device seen yet");
        return false;
    }
    static const std::wstring dir = GameDir();
    const std::wstring path = dir + L"sl.interposer.dll";
    if (!sl::security::verifyEmbeddedSignature(path.c_str())) {
        SetStatus("sl.interposer.dll is not signed by NVIDIA, not loaded");
        return false;
    }
    g_sl.module = LoadLibraryW(path.c_str());
    if (!g_sl.module) {
        SetStatus("could not load sl.interposer.dll (error %lu)", GetLastError());
        return false;
    }
#define SL_GET(member, name)                                                                     \
    g_sl.member = reinterpret_cast<decltype(g_sl.member)>(GetProcAddress(g_sl.module, #name)); \
    if (!g_sl.member) {                                                                          \
        SetStatus("sl.interposer.dll has no %s", #name);                                        \
        return false;                                                                            \
    }
    SL_GET(init, slInit)
    SL_GET(setD3DDevice, slSetD3DDevice)
    SL_GET(upgrade, slUpgradeInterface)
    SL_GET(newFrameToken, slGetNewFrameToken)
    SL_GET(setConstants, slSetConstants)
    SL_GET(setTag, slSetTagForFrame)
    SL_GET(featureFunction, slGetFeatureFunction)
    SL_GET(isSupported, slIsFeatureSupported)
    SL_GET(isLoaded, slIsFeatureLoaded)
    SL_GET(requirements, slGetFeatureRequirements)
    SL_GET(version, slGetFeatureVersion)
    SL_GET(freeResources, slFreeResources)
#undef SL_GET

    static const sl::Feature features[] = {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
    static const wchar_t* pluginPaths[] = {dir.c_str()};
    sl::Preferences pref;
    pref.showConsole = false;
    pref.logLevel = sl::LogLevel::eDefault;
    pref.pathsToPlugins = pluginPaths;
    pref.numPathsToPlugins = 1;
    pref.logMessageCallback = &OnSlLog;
    // No OTA downloads. (An NVIDIA App DLSS override still swaps in its own plugins.)
    pref.flags = sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eUseFrameBasedResourceTagging |
                 sl::PreferenceFlags::eDisableCLStateTracking;
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = static_cast<uint32_t>(std::size(features));
    pref.engine = sl::EngineType::eUnreal;
    pref.engineVersion = "4.26";
    pref.projectId = kProjectId;
    pref.renderAPI = sl::RenderAPI::eD3D12;
    if (!Check(g_sl.init(pref, sl::kSDKVersion), "slInit")) {
        SetStatus("slInit failed (see log)");
        return false;
    }
    if (!Check(g_sl.setD3DDevice(g_device), "slSetD3DDevice")) {
        SetStatus("slSetD3DDevice failed (see log)");
        return false;
    }
    sl::AdapterInfo adapter;
    adapter.deviceLUID = reinterpret_cast<uint8_t*>(&g_luid);
    adapter.deviceLUIDSizeInBytes = sizeof(LUID);
    for (sl::Feature f : features) {
        bool loaded = false;
        g_sl.isLoaded(f, loaded);
        const sl::Result supported = g_sl.isSupported(f, adapter);
        sl::FeatureVersion v;
        g_sl.version(f, v);
        LOG("fg: %s: loaded %d, supported: %s, SL %s, NGX %s", sl::getFeatureAsStr(f), loaded ? 1 : 0,
            sl::getResultAsStr(supported), v.versionSL.toStr().c_str(), v.versionNGX.toStr().c_str());
        if (f == sl::kFeatureDLSS_G && (!loaded || supported != sl::Result::eOk)) {
            SetStatus("DLSS Frame Generation is not supported here (%s)", sl::getResultAsStr(supported));
            return false;
        }
    }
    sl::FeatureRequirements req;
    if (Check(g_sl.requirements(sl::kFeatureDLSS_G, req), "slGetFeatureRequirements(DLSS-G)"))
        LOG("fg: DLSS-G requirements: flags 0x%x (D3D12 %d, VSync off required %d, HW scheduling required %d), "
            "OS %s (needs %s), driver %s (needs %s), %u viewports",
            static_cast<unsigned>(req.flags), (req.flags & sl::FeatureRequirementFlags::eD3D12Supported) ? 1 : 0,
            (req.flags & sl::FeatureRequirementFlags::eVSyncOffRequired) ? 1 : 0,
            (req.flags & sl::FeatureRequirementFlags::eHardwareSchedulingRequired) ? 1 : 0,
            req.osVersionDetected.toStr().c_str(), req.osVersionRequired.toStr().c_str(),
            req.driverVersionDetected.toStr().c_str(), req.driverVersionRequired.toStr().c_str(), req.maxNumViewports);
    if (!GetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", g_sl.dlssgSetOptions) ||
        !GetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState", g_sl.dlssgGetState) ||
        !GetFeatureFunction(sl::kFeatureReflex, "slReflexSetOptions", g_sl.reflexSetOptions) ||
        !GetFeatureFunction(sl::kFeatureReflex, "slReflexSleep", g_sl.reflexSleep) ||
        !GetFeatureFunction(sl::kFeaturePCL, "slPCLSetMarker", g_sl.pclSetMarker)) {
        SetStatus("a DLSS-G / Reflex / PCL function is missing (see log)");
        return false;
    }
    if (!Check(g_sl.newFrameToken(g_token, nullptr), "slGetNewFrameToken") || !g_token) {
        SetStatus("no frame token");
        return false;
    }
    g_state = State::ok;
    SetStatus("Streamline loaded, waiting for the swap chain");
    return true;
}

// Sets DLSS-G (and Reflex, which DLSS-G requires) to <mode>. Present thread, except for the
// resize case, where it has to happen before the resize.
void ApplyMode(Mode mode, const char* why) {
    const bool reflex = g_options.mode != Mode::off;
    if (reflex != g_reflexApplied || g_applied == Mode::count) {
        sl::ReflexOptions o;
        o.mode = reflex ? sl::ReflexMode::eLowLatency : sl::ReflexMode::eOff;
        if (Check(g_sl.reflexSetOptions(o), "slReflexSetOptions")) g_reflexApplied = reflex;
    }
    if (g_applied == Mode::count) {
        sl::DLSSGState state;
        if (Check(g_sl.dlssgGetState(Viewport(), state, nullptr), "slDLSSGGetState")) {
            g_maxFrames = std::max(1u, state.numFramesToGenerateMax);
            g_dynamicSupported = state.bIsDynamicMFGSupported == sl::Boolean::eTrue;
            LOG("fg: up to %u generated frames per rendered frame, dynamic mode %s", g_maxFrames,
                g_dynamicSupported ? "supported" : "not supported (needs multi-frame generation and driver 595.41+)");
        }
    }
    if (mode == Mode::dynamic && !g_dynamicSupported) mode = Mode::x2;
    sl::DLSSGOptions o;
    o.mode = mode == Mode::off       ? sl::DLSSGMode::eOff
             : mode == Mode::dynamic ? sl::DLSSGMode::eDynamic
                                     : sl::DLSSGMode::eOn;
    const uint32_t frames = mode == Mode::x4 ? 3u : mode == Mode::x3 ? 2u : 1u;  // ignored in dynamic mode
    o.numFramesToGenerate = std::clamp(frames, 1u, g_maxFrames);
    o.dynamicTargetFrameRate = 0.0f;  // the display's refresh rate
    // Pauses (DLSS SR not running, cuts, resizes) toggle Off often: keep the memory, no stutter.
    o.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
    if (!Check(g_sl.dlssgSetOptions(Viewport(), o), "slDLSSGSetOptions")) return;
    // Choices and long pauses are logged by the caller; brief pauses (cut frames) aren't.
    if (why) LOG("fg: DLSS-G %s (%s)", ModeName(mode), why);
    g_applied = mode;
}

void Marker(sl::PCLMarker m, const sl::FrameToken& t) { Check(g_sl.pclSetMarker(m, t), sl::getPCLMarkerAsStr(m)); }

// ---- frame pacing ------------------------------------------------------------------------
// Intervals between the game's Present calls (real frames) and, if ReShade is below
// Streamline, between all presented frames. CPU timestamps: the pacing Streamline submits,
// not what the display shows (use FrameView for that).

struct Window {
    LARGE_INTEGER last = {};
    std::vector<float> ms;
};
Window g_real, g_shown;
LARGE_INTEGER g_windowStart = {};
uint64_t g_presentedBySl = 0;  // sum of DLSSGState::numFramesActuallyPresented in this window
uint32_t g_taggedFrames = 0;   // TagFrame calls in this window
uint32_t g_pausedFrames = 0;   // presents with DLSS-G paused in this window
std::string g_pacing = "no data yet";

double Ms(LARGE_INTEGER a, LARGE_INTEGER b) {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    return double(b.QuadPart - a.QuadPart) * 1000.0 / double(freq.QuadPart);
}

void Note(Window& w) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (w.last.QuadPart && w.ms.size() < 20000) w.ms.push_back(float(Ms(w.last, now)));
    w.last = now;
}

std::string Summary(std::vector<float>& v, double seconds) {
    if (v.empty()) return "none";
    double sum = 0, sq = 0;
    for (float x : v) sum += x, sq += double(x) * x;
    const double mean = sum / v.size(), sd = std::sqrt(std::max(0.0, sq / v.size() - mean * mean));
    std::sort(v.begin(), v.end());
    const float p99 = v[std::min(v.size() - 1, size_t(v.size() * 0.99))];
    const size_t hitches = size_t(std::count_if(v.begin(), v.end(), [&](float x) { return x > 1.5 * mean; }));
    char buf[160];
    snprintf(buf, sizeof(buf), "%.1f fps, %.2f ms mean, sd %.2f, p99 %.2f, max %.2f, %zu over 1.5x mean",
             v.size() / seconds, mean, sd, p99, v.back(), hitches);
    return buf;
}

// Every present (present thread): DLSS-G state, and every ~2 s the pacing of the last window.
void PollState() {
    sl::DLSSGState state;
    if (Check(g_sl.dlssgGetState(Viewport(), state, nullptr), "slDLSSGGetState")) {
        g_presentedBySl += state.numFramesActuallyPresented;
        if (uint32_t(state.status) != g_dlssgStatus) {
            g_dlssgStatus = uint32_t(state.status);
            const auto s = state.status;
            LOG("fg: DLSS-G status 0x%x%s%s%s%s%s%s (min size %u, max generated %u)", g_dlssgStatus,
                g_dlssgStatus == 0 ? " ok" : "",
                (s & sl::DLSSGStatus::eFailResolutionTooLow) ? " resolution-too-low" : "",
                (s & sl::DLSSGStatus::eFailReflexNotDetectedAtRuntime) ? " reflex-not-detected" : "",
                (s & sl::DLSSGStatus::eFailHDRFormatNotSupported) ? " hdr-format-not-supported" : "",
                (s & sl::DLSSGStatus::eFailCommonConstantsInvalid) ? " constants-invalid" : "",
                (s & sl::DLSSGStatus::eFailGetCurrentBackBufferIndexNotCalled) ? " no-GetCurrentBackBufferIndex" : "",
                state.minWidthOrHeight, state.numFramesToGenerateMax);
            if (g_dlssgStatus == 0)
                SetStatus("DLSS-G running (status ok)");
            else
                SetStatus("DLSS-G reports a problem: status 0x%x (see log)", g_dlssgStatus);
        }
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!g_windowStart.QuadPart) g_windowStart = now;
    const double seconds = Ms(g_windowStart, now) / 1000.0;
    if (seconds < 2.0) return;
    std::lock_guard lock(g_mutex);
    const size_t real = g_real.ms.size();
    const double perReal = real ? double(g_presentedBySl) / real : 0.0;
    char buf[700];
    snprintf(buf, sizeof(buf),
             "%.1f s: real frames %s | below SL %s | SL presented %llu (%.2f per real frame), tagged %u, paused %u, "
             "DLSS-G status 0x%x",
             seconds, Summary(g_real.ms, seconds).c_str(), Summary(g_shown.ms, seconds).c_str(),
             static_cast<unsigned long long>(g_presentedBySl), perReal, g_taggedFrames, g_pausedFrames, g_dlssgStatus);
    if (g_options.verboseLog) LOG("fg: %s", buf);
    g_pacing = buf;
    {
        char run[96];
        snprintf(run, sizeof(run), "%s, %.0f -> %.0f fps", ModeName(g_applied), real / seconds,
                 g_presentedBySl / seconds);
        std::lock_guard statusLock(g_statusMutex);
        g_running = run;
    }
    g_real.ms.clear();
    g_shown.ms.clear();
    g_presentedBySl = 0;
    g_taggedFrames = 0;
    g_pausedFrames = 0;
    g_windowStart = now;
}

// Around the game's Present: what DLSS-G should do for this frame, PCL markers for it,
// then a new frame token, the Reflex sleep and the next frame's start markers (as close
// as an add-on gets).
template <class F>
HRESULT PresentWithMarkers(F&& present) {
    if (g_state != State::ok) return present();
    sl::FrameToken* token = nullptr;
    bool dirty = false, resizing = false, tagged = false;
    {
        std::lock_guard lock(g_mutex);
        token = g_token;
        dirty = g_optionsDirty;
        g_optionsDirty = false;
        tagged = g_taggedFrame == static_cast<uint32_t>(*token);
        if (g_resizeFrames) {
            --g_resizeFrames;
            resizing = true;
        }
        Note(g_real);
    }
    // No tags for this frame (DLSS SR off or fell back: loading, camera cuts): nothing to
    // generate from, so pause DLSS-G rather than let it complain every frame.
    // Short gaps (a camera-cut frame) aren't logged; a pause of kPauseLogFrames or more is.
    const uint32_t wasUntagged = g_untagged;
    g_untagged = tagged ? 0 : g_untagged + 1;
    if (g_untagged == kPauseLogFrames && g_options.mode != Mode::off)
        LOG("fg: paused: DLSS Super Resolution isn't running");
    if (dirty) LOG("fg: frame generation %s selected", ModeName(g_options.mode));
    Mode want = g_options.mode;
    if (resizing || !tagged || g_freePending) want = Mode::off;
    if (want == Mode::off && g_options.mode != Mode::off) {
        std::lock_guard lock(g_mutex);
        ++g_pausedFrames;
    }
    if (want != g_applied || dirty)
        ApplyMode(want, want != Mode::off && wasUntagged >= kPauseLogFrames ? "resumed" : nullptr);
    Marker(sl::PCLMarker::eRenderSubmitEnd, *token);
    Marker(sl::PCLMarker::ePresentStart, *token);
    ++g_realPresents;  // before: in case ReShade sees this frame's presents inside the call
    const HRESULT hr = present();
    Marker(sl::PCLMarker::ePresentEnd, *token);
    if (g_freePending && g_applied == Mode::off) {
        g_freePending = false;
        if (Check(g_sl.freeResources(sl::kFeatureDLSS_G, Viewport()), "slFreeResources(DLSS-G)"))
            LOG("fg: DLSS-G resources freed after the resize");
    }
    PollState();
    sl::FrameToken* next = nullptr;
    if (Check(g_sl.newFrameToken(next, nullptr), "slGetNewFrameToken") && next) {
        Check(g_sl.reflexSleep(*next), "slReflexSleep");
        Marker(sl::PCLMarker::eSimulationStart, *next);
        Marker(sl::PCLMarker::eSimulationEnd, *next);
        Marker(sl::PCLMarker::eRenderSubmitStart, *next);
        std::lock_guard lock(g_mutex);
        g_token = next;
    }
    return hr;
}

// ---- swap chain hooks --------------------------------------------------------------------

using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                             IDXGISwapChain1**);
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using SetFullscreenStateFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, BOOL, IDXGIOutput*);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using ResizeBuffers1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*,
                                                     IUnknown* const*);

CreateSwapChainFn g_createSwapChain = nullptr;
CreateSwapChainForHwndFn g_createSwapChainForHwnd = nullptr;
// The SL proxy swap chain's own functions.
PresentFn g_slPresent = nullptr;
Present1Fn g_slPresent1 = nullptr;
SetFullscreenStateFn g_slSetFullscreenState = nullptr;
ResizeBuffersFn g_slResizeBuffers = nullptr;
ResizeBuffers1Fn g_slResizeBuffers1 = nullptr;
IDXGIFactory2* g_slFactory = nullptr;  // SL proxy of g_slFactoryBase
IUnknown* g_slFactoryBase = nullptr;
IDXGISwapChain* g_slSwapchain = nullptr;  // SL proxy swap chain ReShade presents through

IDXGIFactory2* SlFactory(IDXGIFactory* native) {
    if (!EnsureSl()) return nullptr;
    if (g_slFactory && g_slFactoryBase == native) return g_slFactory;
    IDXGIFactory2* base = nullptr;
    if (FAILED(native->QueryInterface(IID_PPV_ARGS(&base)))) return nullptr;  // the reference goes to the proxy
    void* p = base;
    if (!Check(g_sl.upgrade(&p), "slUpgradeInterface(IDXGIFactory)") || p == base) {
        LOG("fg: Streamline returned no factory proxy");
        base->Release();
        return nullptr;
    }
    g_slFactory = static_cast<IDXGIFactory2*>(p);
    g_slFactoryBase = native;
    LOG("fg: factory upgraded to an SL proxy");
    return g_slFactory;
}

HRESULT STDMETHODCALLTYPE SlProxyPresent(IDXGISwapChain* self, UINT sync, UINT flags) {
    if (flags & DXGI_PRESENT_TEST) return g_slPresent(self, sync, flags);
    return PresentWithMarkers([&] { return g_slPresent(self, sync, flags); });
}

HRESULT STDMETHODCALLTYPE SlProxyPresent1(IDXGISwapChain1* self, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    if (flags & DXGI_PRESENT_TEST) return g_slPresent1(self, sync, flags, p);
    return PresentWithMarkers([&] { return g_slPresent1(self, sync, flags, p); });
}

void UpdateBackBufferSize(IDXGISwapChain* sc) {
    DXGI_SWAP_CHAIN_DESC d = {};
    if (FAILED(sc->GetDesc(&d))) return;
    std::lock_guard lock(g_mutex);
    g_backBufferW = d.BufferDesc.Width;
    g_backBufferH = d.BufferDesc.Height;
}

// NVIDIA: DLSS-G must be off while the swap chain is resized or changes fullscreen state,
// or Present can deadlock. Switch it off now, free its resources after the next present,
// and keep it off for kResizeFrames presents.
void OffForResize(const char* what) {
    if (g_state != State::ok) return;
    {
        std::lock_guard lock(g_mutex);
        g_resizeFrames = kResizeFrames;
    }
    g_freePending = true;
    if (g_applied != Mode::off && g_applied != Mode::count) ApplyMode(Mode::off, nullptr);
    LOG("fg: DLSS-G off for %s", what);
}

HRESULT STDMETHODCALLTYPE SlProxySetFullscreenState(IDXGISwapChain* self, BOOL fs, IDXGIOutput* target) {
    OffForResize(fs ? "a switch to fullscreen" : "a switch to windowed");
    const HRESULT hr = g_slSetFullscreenState(self, fs, target);
    UpdateBackBufferSize(self);
    return hr;
}

HRESULT STDMETHODCALLTYPE SlProxyResizeBuffers(IDXGISwapChain* self, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags) {
    OffForResize("a swap chain resize");
    const HRESULT hr = g_slResizeBuffers(self, n, w, h, f, flags);
    UpdateBackBufferSize(self);
    return hr;
}

HRESULT STDMETHODCALLTYPE SlProxyResizeBuffers1(IDXGISwapChain3* self, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags,
                                                const UINT* masks, IUnknown* const* queues) {
    OffForResize("a swap chain resize");
    const HRESULT hr = g_slResizeBuffers1(self, n, w, h, f, flags, masks, queues);
    UpdateBackBufferSize(self);
    return hr;
}

void AttachToSlSwapchain(IDXGISwapChain* sc) {
    g_slSwapchain = sc;
    PatchSlot(sc, 8, &SlProxyPresent, g_slPresent, "SL IDXGISwapChain::Present");
    PatchSlot(sc, 10, &SlProxySetFullscreenState, g_slSetFullscreenState, "SL IDXGISwapChain::SetFullscreenState");
    PatchSlot(sc, 13, &SlProxyResizeBuffers, g_slResizeBuffers, "SL IDXGISwapChain::ResizeBuffers");
    PatchSlot(sc, 22, &SlProxyPresent1, g_slPresent1, "SL IDXGISwapChain1::Present1");
    PatchSlot(sc, 39, &SlProxyResizeBuffers1, g_slResizeBuffers1, "SL IDXGISwapChain3::ResizeBuffers1");
    UpdateBackBufferSize(sc);
    std::lock_guard lock(g_mutex);
    g_optionsDirty = true;
    SetStatus("swap chain created through Streamline");
}

// The game's swap chain creation, from the game or (as in The Quarry) from ReShade, goes
// through the SL factory proxy. Streamline's own call back down passes through.
template <class Create>
HRESULT CreateThroughSl(IDXGIFactory* self, const void* ret, const char* what, IDXGISwapChain** out, Create&& create) {
    if (t_inside || !g_hooked || !(FromGame(ret) || FromReShade(ret))) return E_NOTIMPL;
    g_reshadeAbove = FromReShade(ret);
    IDXGIFactory2* proxy = SlFactory(self);
    if (!proxy) return E_NOTIMPL;
    HRESULT hr;
    {
        Inside guard;
        hr = create(proxy);
    }
    LOG("fg: %s through Streamline: 0x%08x", what, static_cast<unsigned>(hr));
    if (SUCCEEDED(hr) && out && *out) AttachToSlSwapchain(*out);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateSwapChainForHwnd(IDXGIFactory2* self, IUnknown* queue, HWND hwnd,
                                                     const DXGI_SWAP_CHAIN_DESC1* desc,
                                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* output,
                                                     IDXGISwapChain1** out) {
    const void* ret = _ReturnAddress();
    if (!t_inside && desc)
        LOG("fg: CreateSwapChainForHwnd from %s: %ux%u format %d, %u buffers, swap effect %d, flags 0x%x",
            Caller(ret).c_str(), desc->Width, desc->Height, static_cast<int>(desc->Format), desc->BufferCount,
            static_cast<int>(desc->SwapEffect), desc->Flags);
    const HRESULT hr = CreateThroughSl(self, ret, "CreateSwapChainForHwnd", reinterpret_cast<IDXGISwapChain**>(out),
                                       [&](IDXGIFactory2* proxy) {
                                           return proxy->CreateSwapChainForHwnd(queue, hwnd, desc, fs, output, out);
                                       });
    return hr != E_NOTIMPL ? hr : g_createSwapChainForHwnd(self, queue, hwnd, desc, fs, output, out);
}

HRESULT STDMETHODCALLTYPE HookCreateSwapChain(IDXGIFactory* self, IUnknown* queue, DXGI_SWAP_CHAIN_DESC* desc,
                                              IDXGISwapChain** out) {
    const void* ret = _ReturnAddress();
    if (!t_inside && desc)
        LOG("fg: CreateSwapChain from %s: %ux%u format %d, %u buffers, swap effect %d, flags 0x%x",
            Caller(ret).c_str(), desc->BufferDesc.Width, desc->BufferDesc.Height,
            static_cast<int>(desc->BufferDesc.Format), desc->BufferCount, static_cast<int>(desc->SwapEffect), desc->Flags);
    const HRESULT hr = CreateThroughSl(self, ret, "CreateSwapChain", out,
                                       [&](IDXGIFactory2* proxy) { return proxy->CreateSwapChain(queue, desc, out); });
    return hr != E_NOTIMPL ? hr : g_createSwapChain(self, queue, desc, out);
}

// Patches the vtable every DXGI factory shares (from system32 dxgi.dll, not ReShade's).
void InstallFactoryHook() {
    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    HMODULE dxgi = LoadLibraryW((std::wstring(sys) + L"\\dxgi.dll").c_str());
    using CreateFactory2Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
    auto create = dxgi ? reinterpret_cast<CreateFactory2Fn>(GetProcAddress(dxgi, "CreateDXGIFactory2")) : nullptr;
    IDXGIFactory2* factory = nullptr;
    if (!create || FAILED(create(0, IID_PPV_ARGS(&factory)))) {
        SetStatus("could not create a DXGI factory to hook");
        return;
    }
    g_hooked = PatchSlot(factory, 10, &HookCreateSwapChain, g_createSwapChain, "IDXGIFactory::CreateSwapChain") &&
               PatchSlot(factory, 15, &HookCreateSwapChainForHwnd, g_createSwapChainForHwnd,
                         "IDXGIFactory2::CreateSwapChainForHwnd");
    factory->Release();
    if (g_hooked) SetStatus("factory hooked, waiting for the game's swap chain");
}

// ---- matrices ----------------------------------------------------------------------------

bool Invert(const float* m, float* out) {
    double a[4][8];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 8; ++c) a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r ? 1.0 : 0.0);
    for (int c = 0; c < 4; ++c) {
        int pivot = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::fabs(a[r][c]) > std::fabs(a[pivot][c])) pivot = r;
        if (std::fabs(a[pivot][c]) < 1e-12) return false;
        if (pivot != c)
            for (int k = 0; k < 8; ++k) std::swap(a[c][k], a[pivot][k]);
        const double inv = 1.0 / a[c][c];
        for (int k = 0; k < 8; ++k) a[c][k] *= inv;
        for (int r = 0; r < 4; ++r)
            if (r != c && a[r][c] != 0.0) {
                const double f = a[r][c];
                for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k];
            }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[r * 4 + c] = float(a[r][c + 4]);
    return true;
}

sl::float4x4 ToSl(const float* m) {
    sl::float4x4 r;
    for (uint32_t i = 0; i < 4; ++i) r.setRow(i, sl::float4(m[i * 4], m[i * 4 + 1], m[i * 4 + 2], m[i * 4 + 3]));
    return r;
}

sl::float3 Normalize(float x, float y, float z) {
    const float l = std::sqrt(x * x + y * y + z * z);
    return l > 0 ? sl::float3(x / l, y / l, z / l) : sl::float3(1, 0, 0);
}

}  // namespace

const char* ModeName(Mode m) {
    static const char* names[] = {"Off", "2x", "3x", "4x", "Dynamic"};
    return m >= Mode::off && m < Mode::count ? names[static_cast<int>(m)] : "?";
}

void LoadSettings() {
    char name[16] = {};
    size_t size = sizeof(name);
    if (reshade::get_config_value(nullptr, kSection, "FrameGenerationMode", name, &size)) {
        for (int i = 0; i < static_cast<int>(Mode::count); ++i)
            if (strcmp(name, ModeName(static_cast<Mode>(i))) == 0) g_options.mode = static_cast<Mode>(i);
    } else {
        // 1.0.0-fg.1/2 test builds: a hook choice, an on/off flag and a frame count.
        int hook = 0;
        bool on = true;
        unsigned int frames = 1;
        if (reshade::get_config_value(nullptr, kSection, "FrameGeneration", hook) && hook != 0) {
            reshade::get_config_value(nullptr, kSection, "FrameGenerationOn", on);
            reshade::get_config_value(nullptr, kSection, "FrameGenerationFrames", frames);
            g_options.mode = !on ? Mode::off : frames >= 3 ? Mode::x4 : frames == 2 ? Mode::x3 : Mode::x2;
        }
    }
    reshade::get_config_value(nullptr, kSection, "FrameGenerationCopyTags", g_options.copyTags);
    const std::wstring dir = GameDir();
    g_filesPresent = true;
    for (const wchar_t* f : kFiles)
        if (GetFileAttributesW((dir + f).c_str()) == INVALID_FILE_ATTRIBUTES) {
            if (g_options.mode != Mode::off) LOG("fg: %ls not found next to the game exe", f);
            g_filesPresent = false;
        }
    LOG("fg: settings: frame generation %s, copy tags %d, Streamline files %s", ModeName(g_options.mode),
        g_options.copyTags, g_filesPresent ? "present" : "missing");
}

void SaveSettings() {
    reshade::set_config_value(nullptr, kSection, "FrameGenerationMode", ModeName(g_options.mode));
    reshade::set_config_value(nullptr, kSection, "FrameGenerationCopyTags", g_options.copyTags);
    std::lock_guard lock(g_mutex);
    g_optionsDirty = true;
}

void OnInitDevice(reshade::api::device* dev) {
    if (g_options.mode == Mode::off || !g_filesPresent || dev->get_api() != reshade::api::device_api::d3d12) return;
    g_device = reinterpret_cast<ID3D12Device*>(dev->get_native());
    g_luid = g_device->GetAdapterLuid();
    if (!g_hooked) InstallFactoryHook();
}

bool IsRealPresent() {
    if (!g_slSwapchain || g_reshadeAbove) return true;
    {
        std::lock_guard lock(g_mutex);
        Note(g_shown);
    }
    const uint64_t real = g_realPresents.load();
    if (g_countedPresents >= real) return false;
    g_countedPresents = real;
    return true;
}

void TagFrame(const FrameInputs& in) {
    if (g_state != State::ok || !g_slSwapchain || !in.renderW || !in.renderH) return;
    sl::FrameToken* token;
    uint32_t bbW, bbH;
    {
        std::lock_guard lock(g_mutex);
        token = g_token;
        bbW = g_backBufferW ? g_backBufferW : in.backBufferW;
        bbH = g_backBufferH ? g_backBufferH : in.backBufferH;
    }

    sl::Constants c;
    float viewToClip[16], clipToView[16], prevClipToClip[16];
    memcpy(viewToClip, in.viewToClip, sizeof(viewToClip));
    viewToClip[8] = viewToClip[9] = 0.0f;  // UE adds the TAA jitter to row 2 .xy; SL wants it separately
    if (!Invert(viewToClip, clipToView) || !Invert(in.clipToPrevClip, prevClipToClip)) {
        if (++g_errors <= 60) LOG("fg: a camera matrix is not invertible, frame not tagged");
        return;
    }
    static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    c.cameraViewToClip = ToSl(viewToClip);
    c.clipToCameraView = ToSl(clipToView);
    c.clipToLensClip = ToSl(identity);
    c.clipToPrevClip = ToSl(in.clipToPrevClip);
    c.prevClipToClip = ToSl(prevClipToClip);
    c.jitterOffset = sl::float2(in.jitterX, in.jitterY);
    const float sign = in.invertMotion ? -1.0f : 1.0f;  // our motion is in render pixels
    c.mvecScale = sl::float2(sign / float(in.renderW), sign / float(in.renderH));
    c.cameraPinholeOffset = sl::float2(0, 0);
    c.cameraPos = sl::float3(in.cameraPos[0], in.cameraPos[1], in.cameraPos[2]);
    // UE: X forward, Y right, Z up (left-handed). Roll is ignored.
    c.cameraFwd = Normalize(in.cameraFwd[0], in.cameraFwd[1], in.cameraFwd[2]);
    c.cameraRight = Normalize(-c.cameraFwd.y, c.cameraFwd.x, 0.0f);
    c.cameraUp = sl::float3(c.cameraFwd.y * c.cameraRight.z - c.cameraFwd.z * c.cameraRight.y,
                            c.cameraFwd.z * c.cameraRight.x - c.cameraFwd.x * c.cameraRight.z,
                            c.cameraFwd.x * c.cameraRight.y - c.cameraFwd.y * c.cameraRight.x);
    c.cameraNear = viewToClip[14] > 0 ? viewToClip[14] : 10.0f;  // row 3.z: near plane, UE cm
    c.cameraFar = 1.0e7f;                                        // infinite far plane
    c.cameraFOV = 2.0f * std::atan(1.0f / viewToClip[5]);        // vertical
    c.cameraAspectRatio = viewToClip[5] / viewToClip[0];
    c.depthInverted = sl::Boolean::eTrue;
    c.cameraMotionIncluded = sl::Boolean::eTrue;
    c.motionVectors3D = sl::Boolean::eFalse;
    c.reset = in.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    c.orthographicProjection = sl::Boolean::eFalse;
    c.motionVectorsDilated = sl::Boolean::eFalse;
    c.motionVectorsJittered = in.mvJittered ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    if (!Check(g_sl.setConstants(c, *token, Viewport()), "slSetConstants")) return;

    const auto life = g_options.copyTags ? sl::ResourceLifecycle::eOnlyValidNow : sl::ResourceLifecycle::eValidUntilPresent;
    sl::Resource depth(sl::ResourceType::eTex2d, in.depth, nullptr, nullptr, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    sl::Resource motion(sl::ResourceType::eTex2d, in.motion, nullptr, nullptr, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const sl::Extent render{0, 0, in.renderW, in.renderH};
    // Letterboxed shots: the view sits centred in the back buffer (3840x1600 at y 280 in 2160).
    sl::Extent screen{0, 0, bbW, bbH};
    if (bbW >= in.outW && bbH >= in.outH && in.outW && in.outH)
        screen = {(bbH - in.outH) / 2, (bbW - in.outW) / 2, in.outW, in.outH};
    const sl::ResourceTag tags[] = {
        sl::ResourceTag(&depth, sl::kBufferTypeDepth, life, &render),
        sl::ResourceTag(&motion, sl::kBufferTypeMotionVectors, life, &render),
        sl::ResourceTag(nullptr, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle::eValidUntilPresent, &screen),
    };
    const uint32_t count = screen ? 3u : 2u;
    if (!Check(g_sl.setTag(*token, Viewport(), tags, count, in.cmd), "slSetTagForFrame")) return;
    {
        std::lock_guard lock(g_mutex);
        g_taggedFrame = static_cast<uint32_t>(*token);
        ++g_taggedFrames;
    }
    static uint32_t logged = 0;
    if (++logged == 1 || (g_options.verboseLog && in.reset && logged < 200))
        LOG("fg: tagged frame %u: render %ux%u, back buffer subrect %u,%u %ux%u, near %.2f, fov %.1f deg, aspect %.3f%s",
            static_cast<uint32_t>(*token), in.renderW, in.renderH, screen.left, screen.top, screen.width, screen.height,
            c.cameraNear, c.cameraFOV * 57.29578f, c.cameraAspectRatio, in.reset ? ", reset" : "");
}

void Shutdown() { UnpatchAll(); }

bool Loaded() { return g_state == State::ok; }

bool Selectable() { return g_filesPresent && g_state != State::failed; }

std::string FriendlyStatus() {
    if (!g_filesPresent)
        return "Not installed: needs the Streamline files next to the game exe (see the README).";
    if (g_options.mode == Mode::off) return g_state == State::ok ? "Off." : "";
    if (!g_hooked) return "Restart the game to turn on frame generation.";
    std::lock_guard lock(g_statusMutex);
    if (g_state == State::failed) return "Not available: " + g_status + ".";
    if (g_state != State::ok || !g_slSwapchain) return "Starting: load into the game.";
    if (g_dlssgStatus != 0 && g_dlssgStatus != 0xffffffffu) return "Problem: " + g_status + ".";
    if (g_untagged >= kPauseLogFrames) return "Paused: needs DLSS Super Resolution running.";
    std::string s = "Active: " + (g_running.empty() ? std::string(ModeName(g_options.mode)) : g_running) + ".";
    if (g_options.mode == Mode::dynamic && !g_dynamicSupported) s += " Dynamic isn't supported here, so 2x.";
    return s;
}

std::string Status() {
    std::lock_guard lock(g_statusMutex);
    return g_status;
}

std::string Pacing() {
    std::lock_guard lock(g_mutex);
    return g_pacing;
}

}  // namespace fg
