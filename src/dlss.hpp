// DLSS Super Resolution for The Quarry on Direct3D 12, recorded into the game's own
// command list in place of its TAAU dispatch. See dlss.cpp.
#pragma once

#include <reshade.hpp>

#include <cstdint>
#include <string>

namespace dlss {

using namespace reshade::api;

// Everything DLSS needs from one TAAU dispatch.
struct Frame {
    command_list* cmd = nullptr;
    resource color, depth, velocity, output;  // game textures, already in SRV / UAV state
    uint32_t renderW = 0, renderH = 0;         // render view rect (top-left 0,0)
    uint32_t outX = 0, outY = 0, outW = 0, outH = 0;  // output view rect
    float jitterX = 0, jitterY = 0;            // render pixels, from the TAA constants
    float clipToPrevClip[16] = {};             // View.ClipToPrevClip, row-major
    bool reset = false;                        // camera cut (sticky across fallback frames)
    uint64_t frame = 0;                        // probe frame number, to spot frames DLSS missed
};

enum class Result {
    skipped,  // nothing recorded: let the game's TAA run
    failed,   // commands were recorded but DLSS failed: restore state, let the game's TAA run
    done,     // DLSS wrote the output: restore state, skip the game's TAA
};

struct Options {
    bool enabled = true;
    bool flipJitterX = false;
    bool flipJitterY = false;
    bool mvJittered = false;  // DLSS feature flag; changing it recreates the feature
    bool invertMotion = false;  // DLSS MV scale -1: tests the motion vector sign
    bool ignoreVelocity = false;  // camera reprojection only: tests the game velocity decode
    bool clampLightingHistory = true;  // give SSGI/SSR a clamped copy of the DLSS output
};
extern Options g_options;

// Every present: NGX init and DLSS feature (re)creation happen here, on ReShade's
// immediate command list, never inside the game's command lists.
void Tick(effect_runtime* runtime);
// At a TAAU dispatch: records DLSS only if a feature for exactly these sizes exists;
// otherwise asks Tick for one and returns skipped without touching the command list.
Result Evaluate(const Frame& f);
// Before a pass that reads <texture>: if it's last frame's DLSS output (read back by the
// game's SSGI/SSR), copy the clamped version over it first. Once per frame.
bool PatchLightingHistory(command_list* cmd, resource texture, bool compute, uint64_t frame);
uint64_t LightingHistoryPatches();
void OnDestroyResource(resource r);  // drops cached views of game textures
void Shutdown(device* dev);

std::string Status();  // one line for the overlay and logs
uint64_t FramesDone();

// Set by the probe: writes a line to QuarryDLSS.log.
extern void (*g_log)(const char* fmt, ...);

}  // namespace dlss
