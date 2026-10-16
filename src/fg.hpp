// DLSS Frame Generation (DLSS-G through NVIDIA Streamline) experiment, off by default.
// See fg.cpp and docs/FINDINGS.md, "Frame Generation spike".
#pragma once

#include <reshade.hpp>

#include <cstdint>
#include <string>

struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace fg {

// How Streamline's swap chain proxy is put in the game's present path. Read from
// ReShade.ini at load: a change needs a game restart.
enum class Hook : int {
    off = 0,
    factory = 1,  // swap chain creation goes through an SL factory proxy; ReShade makes that call,
                  // so game -> ReShade -> SL -> DXGI (works, FINDINGS.md run 2)
    upgrade = 2,  // slUpgradeInterface on the finished swap chain, ReShade's calls redirected to it
                  // (game -> ReShade -> SL -> DXGI); needs a swap chain resize to take the buffers over
};

// What DLSS-G presents per rendered frame. DLSS-G has no model presets to choose
// (Streamline only has them for Super Resolution and Ray Reconstruction); its model
// comes from the driver or the NVIDIA App override.
enum class Mode : int {
    off = 0,   // DLSSGMode::eOff (Streamline stays loaded)
    x2,        // eOn, 1 generated frame
    x3,        // eOn, 2
    x4,        // eOn, 3 (capped by numFramesToGenerateMax)
    dynamic,   // eDynamic: Streamline picks the multiplier to reach the refresh rate
    count
};
const char* ModeName(Mode m);  // "Off", "2x", ... (also the ReShade.ini value)

struct Options {
    Hook hook = Hook::off;
    Mode mode = Mode::x2;
    bool reflex = true;              // Reflex low latency (DLSS-G refuses to run without it)
    bool copyTags = true;            // eOnlyValidNow (SL copies motion/depth when tagged), else eValidUntilPresent
};
extern Options g_options;

// Everything DLSS-G needs from one frame, taken where DLSS Super Resolution runs.
struct FrameInputs {
    ID3D12GraphicsCommandList* cmd = nullptr;  // native, the game's
    ID3D12Resource* motion = nullptr;          // render res RG16F, DLSS pixel convention, in UAV state
    ID3D12Resource* depth = nullptr;           // render res R32F, reversed-Z, in UAV state
    uint32_t renderW = 0, renderH = 0;
    uint32_t outW = 0, outH = 0;               // view rect at output resolution
    uint32_t backBufferW = 0, backBufferH = 0;
    float jitterX = 0, jitterY = 0;            // render pixels, as given to DLSS
    float viewToClip[16] = {};                 // View rows 28-31 (jitter in row 30.xy)
    float clipToPrevClip[16] = {};             // View rows 122-125 (no jitter)
    float cameraPos[3] = {};                   // View row 67, UE cm
    float cameraFwd[3] = {};                   // View row 18
    bool reset = false;
    bool mvJittered = false;
    bool invertMotion = false;
};

void LoadSettings();
void SaveSettings();
void OnInitDevice(reshade::api::device* dev);   // remembers the device; installs the factory hook
void OnInitSwapchain(reshade::api::swapchain* sc, bool resize);
void Tick();                                    // every ReShade present (the upgrade hook happens here)
// Called by the probe's present event: false for the frames DLSS-G generated, which
// ReShade also sees with the factory hook. The probe counts frames on true only.
bool IsRealPresent();
void TagFrame(const FrameInputs& in);           // after DLSS SR, on the game's command list
void Shutdown();                                // restores every patched vtable entry

std::string Status();          // one line for the overlay
std::string Pacing();          // last measured window
bool Loaded();                 // Streamline initialised

extern void (*g_log)(const char* fmt, ...);

}  // namespace fg
