# DarkPicturesDLSS

NVIDIA **DLSS Super Resolution** and **DLSS Frame Generation** for **The Dark Pictures
Anthology** on PC, as a ReShade add-on. It replaces the game's own temporal upscaler with
DLSS and can add generated frames on top through NVIDIA Streamline. It's the Dark Pictures
build of [QuarryDLSS](https://github.com/GreatDanish1026/The-Quarry---DLSS) (same source,
same Unreal Engine 4.26 renderer).

Version **0.1.0**. Supported game: **House of Ashes**. Little Hope, The Devil in Me and Man
of Medan aren't supported yet.

## Requirements

- An NVIDIA RTX graphics card and a current driver. Frame generation needs an RTX 40 or 50
  series card; 3x, 4x and Dynamic need an RTX 50 series card.
- [ReShade](https://reshade.me) **6.8 or newer, the version with full add-on support**.
- The game running on **DirectX 12** (House of Ashes does by default).
- For frame generation: **Hardware-accelerated GPU scheduling** on (Windows Settings >
  System > Display > Graphics).

## Install

1. Install ReShade with add-on support for the game's shipping executable, for House of
   Ashes `SMG025\Binaries\Win64\HouseOfAshes-Win64-Shipping.exe` (not the `HouseOfAshes.exe`
   launcher). Choose DirectX 10/11/12. No effect packs are needed.
2. Copy **all the files at the top level of the zip** into that same `Win64` folder:
   - `DarkPicturesDLSS.addon64`, the add-on;
   - `nvngx_dlss.dll`, NVIDIA's DLSS Super Resolution runtime (310.9.1);
   - `nvngx_dlssg.dll`, `sl.interposer.dll`, `sl.common.dll`, `sl.dlss_g.dll`,
     `sl.reflex.dll` and `sl.pcl.dll`, NVIDIA's DLSS Frame Generation runtime (Streamline
     2.14.1). They're only loaded while frame generation is on.

   The `licenses` folder and the text files are for reference; they don't need copying.
3. **In the NVIDIA App, turn off any DLSS Super Resolution model override for the game.**
   It would replace the model this add-on asks for.

## Use

Press **Home** in game to open ReShade, then open the **Add-ons** tab and the
**DarkPicturesDLSS** section:

- **DLSS Super Resolution**: on or off. Off gives you the game's own anti-aliasing at
  native resolution.
- **Quality mode**: Ultra Quality (77%), Quality (67%), Balanced (58%), Performance
  (50%) or Ultra Performance (33%) of the output resolution per axis.
- **Frame generation**: Off, 2x, 3x, 4x or Dynamic. Dynamic picks the smallest multiplier
  that reaches your refresh rate. It needs DLSS Super Resolution on. **Turning it on from
  Off needs a game restart**; changing the mode or turning it off works straight away.
- **Advanced**: the DLSS model (E is the default: J leaves trails behind fast hands in
  dark scenes), a custom render scale, and
  "Stabilise screen-space lighting".
- **Diagnostics**: for troubleshooting and bug reports only.

Settings are saved in `ReShade.ini` under `[DarkPicturesDLSS]`.

## Known limits

- DirectX 12 only. Early build: tested in House of Ashes gameplay and cutscenes with DLSS
  Super Resolution; frame generation is still being tested there.
- Frame generation pauses for a moment on camera cuts, loading screens, window resizes and
  fullscreen switches, and while DLSS Super Resolution is off.
- The frame generation model is NVIDIA's default, or the NVIDIA App's override if you've
  set one. DLSS Frame Generation has no presets to choose.
- During loading and on camera-cut frames, the game's own upscaler is used for a frame.

## Reporting a problem

Send `DarkPicturesDLSS.log` (and `DarkPicturesDLSS.prev.log`) from the `Win64` folder,
plus a screenshot. In Diagnostics, **Verbose log** adds motion statistics, Streamline's own
log lines and frame pacing.

## Building (developers)

```powershell
.\build.ps1 -Product DarkPictures            # -> bin\DarkPicturesDLSS.addon64
.\build.ps1 -Product DarkPictures -Install   # into House of Ashes (or pass -GameBin)
.\build.ps1 -Product DarkPictures -Package   # -> bin\release\DarkPicturesDLSS-<version>.zip
```

See the main README for the build requirements and `docs/FINDINGS.md` for how the
renderer was mapped.

## Credits

This software uses NVIDIA DLSS and NVIDIA Streamline. NVIDIA, DLSS and Reflex are
trademarks of NVIDIA Corporation. See THIRD_PARTY_NOTICES.txt.
