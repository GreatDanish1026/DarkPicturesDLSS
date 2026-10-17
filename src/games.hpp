// The games each product is made for, by shipping executable. The add-on finds the
// renderer's passes by their bindings, so it may work in a game not listed here; the log
// says which game it recognised.
#pragma once

#include "version.h"

struct GameInfo {
    const wchar_t* exe;  // file name of the shipping executable
    const char* name;
    // DLSS model preset hint until the user picks one (dlss::Options::preset; 0 keeps the default).
    unsigned int preset = 0;
};

#if defined(QDLSS_DARK_PICTURES)
constexpr GameInfo kGames[] = {
    // Model E: J leaves striped trails behind fast hands against dark backgrounds, and ignores
    // DLSS's bias-current-colour mask (FINDINGS.md). Seen in both games.
    {L"HouseOfAshes-Win64-Shipping.exe", "House of Ashes", 5},   // UE 4.26, SMG025
    {L"TheDevilInMe-Win64-Shipping.exe", "The Devil in Me", 5},  // UE 4.26, SMG028
    {L"LittleHope-Win64-Shipping.exe", "Little Hope", 5},        // UE 4.26, SMG024 (D3D11 and D3D12 RHIs)
};
#else
constexpr GameInfo kGames[] = {
    {L"TheQuarry-Win64-Shipping.exe", "The Quarry"},  // UE 4.26, SMG026
};
#endif
