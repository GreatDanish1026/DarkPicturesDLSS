# DarkPicturesDLSS

NVIDIA **DLSS Super Resolution** and **DLSS Frame Generation** for **The Dark Pictures
Anthology** on PC, as a ReShade add-on. It replaces the game's own temporal upscaler with
DLSS and can add generated frames on top through NVIDIA Streamline. It's the Dark Pictures
build of [QuarryDLSS](https://github.com/GreatDanish1026/The-Quarry---DLSS): same source,
same Unreal Engine 4.26 renderer.

Version **0.1.8**.

| Game | Status |
|---|---|
| House of Ashes | Supported: DLSS, frame generation, all quality modes |
| The Devil in Me | Supported: DLSS, frame generation, all quality modes |
| Man of Medan, Little Hope | Not supported yet |

## Requirements

- An NVIDIA RTX graphics card and a current driver. Frame generation needs an RTX 40 or 50
  series card; 3x, 4x and Dynamic need an RTX 50 series card.
- [ReShade](https://reshade.me) **6.8 or newer, the version with full add-on support**.
- The game running on **DirectX 12** (both supported games do by default).
- For frame generation: **Hardware-accelerated GPU scheduling** on (Windows Settings >
  System > Display > Graphics).

## Install

1. Install ReShade with add-on support for the game's shipping executable, not its
   launcher. Choose DirectX 10/11/12. No effect packs are needed.

   | Game | Folder and executable |
   |---|---|
   | House of Ashes | `SMG025\Binaries\Win64\HouseOfAshes-Win64-Shipping.exe` |
   | The Devil in Me | `SMG028\Binaries\Win64\TheDevilInMe-Win64-Shipping.exe` |

2. Copy **all the files at the top level of the zip** into that same `Win64` folder:
   - `DarkPicturesDLSS.addon64`, the add-on;
   - `nvngx_dlss.dll`, NVIDIA's DLSS Super Resolution runtime (310.9.1);
   - `nvngx_dlssg.dll`, `sl.interposer.dll`, `sl.common.dll`, `sl.dlss_g.dll`,
     `sl.reflex.dll` and `sl.pcl.dll`, NVIDIA's DLSS Frame Generation runtime (Streamline
     2.14.1). They're only loaded while frame generation is on.

   The `licenses` folder and the text files are for reference; they don't need copying.
3. **In the NVIDIA App, turn off any DLSS Super Resolution model override for the game.**
   It would replace the model this add-on asks for (see Known limits).

## Use

Press **Home** in game to open ReShade, then open the **Add-ons** tab and the
**DarkPicturesDLSS** section:

- **DLSS Super Resolution**: on or off. Off gives you the game's own anti-aliasing at
  native resolution.
- The status line shows whether DLSS is active and at what resolutions.
- **Quality mode**: Ultra Quality (77%), Quality (67%), Balanced (58%), Performance
  (50%) or Ultra Performance (33%) of the output resolution per axis.
- **Frame generation**: Off, 2x, 3x, 4x or Dynamic. 2x to 4x present each rendered frame
  plus one to three generated ones. Dynamic picks the smallest multiplier that reaches
  your refresh rate. Frame generation needs DLSS Super Resolution on; the line under it
  says whether it's active. **Turning it on from Off needs a game restart**; changing the
  mode or turning it off works straight away.
- **Advanced**: the DLSS model (E by default, see Known limits), a custom render scale,
  and "Stabilise screen-space lighting" (only matters with mods that add screen-space
  lighting).
- **Diagnostics**: for troubleshooting and bug reports only.

Settings are saved in `ReShade.ini` under `[DarkPicturesDLSS]`.

## Known limits

- DirectX 12 only.
- The add-on asks for DLSS model **E**. The newer models J and K leave striped trails
  behind fast-moving hands against dark backgrounds, which these games have a lot of.
  An NVIDIA App or driver model override for the game replaces that choice.
- Frame generation pauses for a moment on camera cuts, loading screens, window resizes
  and fullscreen switches, and while DLSS Super Resolution is off.
- The frame generation model is NVIDIA's default, or the NVIDIA App's frame generation
  override if you've set one. DLSS Frame Generation has no presets to choose.
- The ReShade overlay itself is drawn before frame generation, so it's interpolated too.
- During loading and on camera-cut frames, the game's own upscaler is used for a frame.

## Reporting a problem

Send `DarkPicturesDLSS.log` (and `DarkPicturesDLSS.prev.log`) from the `Win64` folder,
plus a screenshot. In Diagnostics, **Verbose log** adds motion statistics, Streamline's own
log lines and frame pacing, which helps with smearing, ghosting or frame generation reports.

## Building (developers)

```powershell
.\build.ps1 -Product DarkPictures                      # -> bin\DarkPicturesDLSS.addon64
.\build.ps1 -Product DarkPictures -Install             # into House of Ashes, plus nvngx_dlss.dll
.\build.ps1 -Product DarkPictures -Install -GameBin "<The Devil in Me>\SMG028\Binaries\Win64"
.\build.ps1 -Product DarkPictures -InstallStreamline   # the frame generation runtime
.\build.ps1 -Product DarkPictures -Package             # -> bin\release\DarkPicturesDLSS-<version>.zip
.\build.ps1 -Test                                      # also builds and runs the DXBC parser tests
```

The same source also builds QuarryDLSS for The Quarry (`.\build.ps1`, the default product;
its README is `docs/README-Quarry.md`). Product names and versions are in `src/version.h`,
the supported games and their default DLSS model in `src/games.hpp`.

Building needs Visual Studio 2022 or newer with the C++ x64 tools, and the Windows SDK's
`fxc.exe`. MinGW builds crash inside ReShade: MinGW returns API structs with a different
calling convention than MSVC. The NVIDIA DLLs for `-Install`, `-InstallStreamline` and
`-Package` come from `bin\x64` of the
[Streamline 2.14.1 release](https://github.com/NVIDIA-RTX/Streamline/releases), copied to
`third_party\streamline\bin\x64` (or pass `-StreamlineBin`). They aren't in this repository.

How the renderer was mapped, and every finding along the way, is in `docs/FINDINGS.md`.
`tools/analyze_capture.py` analyses the texture captures from the Diagnostics section.
It needs numpy and Pillow.

## Credits

This software uses NVIDIA DLSS and NVIDIA Streamline. NVIDIA, DLSS and Reflex are
trademarks of NVIDIA Corporation. See THIRD_PARTY_NOTICES.txt.
