// The games each product is made for, by shipping executable. The add-on finds the
// renderer's passes by their bindings, so it may work in a game not listed here; the log
// says which game it recognised.
#pragma once

#include "version.h"

struct GameInfo {
    const wchar_t* exe;  // file name of the shipping executable
    const char* name;
};

#if defined(QDLSS_DARK_PICTURES)
constexpr GameInfo kGames[] = {
    {L"HouseOfAshes-Win64-Shipping.exe", "House of Ashes"},  // UE 4.26, SMG025
};
#else
constexpr GameInfo kGames[] = {
    {L"TheQuarry-Win64-Shipping.exe", "The Quarry"},  // UE 4.26, SMG026
};
#endif
