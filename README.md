# QuarryDLSS

NVIDIA **DLSS Super Resolution** for **The Quarry** (PC, Steam build 11105655, Unreal
Engine 4.26), as a ReShade add-on. It replaces the game's own temporal upscaler with DLSS,
following the approach of [days-gone-dlss](https://github.com/teemupynnonen/days-gone-dlss).

Version **0.6.1**.

## Requirements

- An NVIDIA RTX graphics card and a current driver.
- [ReShade](https://reshade.me) **6.8 or newer, the version with full add-on support**.
- The game running on **DirectX 12**: add `-dx12` to its Steam launch options.
- `nvngx_dlss.dll` version 310.9.x next to the game executable (see Install).

## Install

1. Install ReShade with add-on support for
   `The Quarry\SMG026\Binaries\Win64\TheQuarry-Win64-Shipping.exe` (not the
   `TheQuarry.exe` launcher). Choose DirectX 10/11/12. No effect packs are needed.
2. Copy `QuarryDLSS.addon64` into that same `Win64` folder.
3. Copy `nvngx_dlss.dll` (310.9.x) into that folder too. It's NVIDIA's DLSS runtime and
   isn't included here. Other DLSS games ship it.
4. In Steam, set the launch options to `-dx12`.
5. **In the NVIDIA App, turn off any DLSS model override for The Quarry.** It would
   replace the model this add-on asks for (see Known limits).

## Use

Press **Home** in game to open ReShade, then open the **Add-ons** tab and the
**QuarryDLSS** section:

- **DLSS Super Resolution**: on or off. Off gives you the game's own anti-aliasing at
  native resolution.
- The status line shows whether DLSS is active and at what resolutions.
- **Quality mode**: Ultra Quality (77%), Quality (67%), Balanced (58%), Performance
  (50%) or Ultra Performance (33%) of the output resolution per axis.
- **Advanced**: the DLSS model (J is recommended), a custom render scale, and
  "Stabilise screen-space lighting", which helps with the SSGI/SSR pak mods.
- **Diagnostics**: for troubleshooting and bug reports only.

Settings are saved in `ReShade.ini` under `[QuarryDLSS]`.

## Known limits

- DirectX 12 only.
- Quality mode is the one that's been tested most. The other modes should work, but
  haven't had a full playthrough.
- DLSS model **K**, which NVIDIA uses by default for Quality, smears some close-up
  cutscene shots, so the add-on asks for **J**. An NVIDIA App or driver model override
  for the game replaces that choice.
- Some cutscene cuts briefly flash white. The game does this itself, and it happens with
  DLSS off too.
- During loading and on camera-cut frames, the game's own upscaler is used for a frame.

## Reporting a problem

Send `QuarryDLSS.log` (and `QuarryDLSS.prev.log`) from the `Win64` folder, plus a
screenshot. In Diagnostics, **Verbose log** adds motion statistics, which helps with
smearing or ghosting reports.

## Building (developers)

```powershell
.\build.ps1            # -> bin\QuarryDLSS.addon64
.\build.ps1 -Test      # also builds and runs the DXBC parser tests
.\build.ps1 -Install   # also copies it (and nvngx_dlss.dll from -DlssDll) into the game folder
```

Building needs Visual Studio 2022 or newer with the C++ x64 tools, and the Windows SDK's
`fxc.exe`. MinGW builds crash inside ReShade: MinGW returns API structs with a
different calling convention than MSVC.

How the renderer was mapped, and every finding along the way, is in `docs/FINDINGS.md`.
`tools/analyze_capture.py` analyses the texture captures from the Diagnostics section.
It needs numpy and Pillow.
