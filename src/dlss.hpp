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
    bool reset = false;                        // camera cut
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
};
extern Options g_options;

Result Evaluate(const Frame& f);
void OnDestroyResource(resource r);  // drops cached views of game textures
void Shutdown(device* dev);

std::string Status();  // one line for the overlay and logs
uint64_t FramesDone();

// Set by the probe: writes a line to QuarryDLSS.log.
extern void (*g_log)(const char* fmt, ...);

}  // namespace dlss
