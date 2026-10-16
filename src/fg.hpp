// DLSS Frame Generation (DLSS-G through NVIDIA Streamline). Off by default. See fg.cpp and
// docs/FINDINGS.md, "Frame Generation spike".
#pragma once

#include <reshade.hpp>

#include <cstdint>
#include <string>

struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace fg {

// What DLSS-G presents per rendered frame. DLSS-G has no model presets to choose
// (Streamline only has them for Super Resolution and Ray Reconstruction); its model
// comes from the driver or the NVIDIA App override.
enum class Mode : int {
    off = 0,   // Streamline isn't loaded if this is the setting at launch
    x2,        // DLSSGMode::eOn, 1 generated frame
    x3,        // eOn, 2
    x4,        // eOn, 3 (capped by numFramesToGenerateMax)
    dynamic,   // eDynamic: Streamline picks the multiplier to reach the refresh rate
    count
};
const char* ModeName(Mode m);  // "Off", "2x", ... (also the ReShade.ini value)

struct Options {
    Mode mode = Mode::off;
    bool copyTags = true;    // eOnlyValidNow (SL copies motion/depth when tagged), else eValidUntilPresent
    bool verboseLog = false; // Streamline's info lines and the 2 s pacing lines (set by the probe)
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
// Installs the swap chain creation hook if frame generation is on at launch and the
// Streamline files are next to the game exe.
void OnInitDevice(reshade::api::device* dev);
// Called by the probe's present event: false for frames DLSS-G generated, in case ReShade
// ever sees them (only if the game created its swap chain without going through ReShade).
bool IsRealPresent();
void TagFrame(const FrameInputs& in);  // after DLSS SR, on the game's command list
void Shutdown();                       // restores every patched vtable entry

bool Selectable();             // the overlay may offer the modes (files present, nothing failed)
std::string FriendlyStatus();  // one plain line for the overlay's main panel
std::string Status();          // technical status, for Diagnostics
std::string Pacing();          // last measured 2 s window, for Diagnostics
bool Loaded();                 // Streamline initialised

extern void (*g_log)(const char* fmt, ...);

}  // namespace fg
