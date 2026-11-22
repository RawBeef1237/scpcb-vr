# SCP - Containment Breach VR

Unofficial OpenXR VR support for SCP - Containment Breach (the SCP-CB-CPP `bx3d` branch, built with a patched
BlitzX3D). Tested on Meta Quest through Virtual Desktop. The code was written with AI assistance.

## Features

- 6DoF stereo rendering with head tracking, through OpenXR (Direct3D 11 bridge, or DXVK/Vulkan)
- Tracked hands (rigged hand models) with finger curl from grip and trigger, and capacitive touch (point with the
  index finger, thumbs up) on Quest Touch and Index controllers
- Blink and stamina meters on the back of the left hand
- HUD floating in front of you; pause menu, inventory, keypads and other screens on a floating panel you use with a
  laser pointer from the controller
- Real-life crouching crouches in game
- Physical interaction: grab and pull levers, turn the SCP-914 key and knob, hold keycards and severed hands and touch
  them to readers and scanners, read documents on a sheet in your hand
- Haptics for grabbing, lever and knob clicks, reader access granted / denied
- Mouse and keyboard keep working alongside the controllers

## Controls (Quest Touch)

| Action | Control |
|---|---|
| Move | Left stick (walks where you look) |
| Turn | Right stick (30 degree snap turn by default) |
| Blink | Left trigger (hold to keep your eyes shut) |
| Interact (doors, items, buttons) | Right trigger |
| Sprint | Left stick click |
| Crouch | A, or crouch in real life |
| Inventory | B |
| Pause menu | Left menu button |
| Save | Y |
| Recenter | Hold the Oculus / system button (done by the VR software) |
| Menus, inventory | Aim a controller at the panel, right trigger clicks |
| Grab levers, SCP-914 key and knob | Grip, with your palm on the part |
| Use a keycard / severed hand | Select it in the inventory, then touch it to the reader / scanner |
| Read a document | Select it in the inventory: it appears in your left hand |

Index, Vive and Windows Mixed Reality bindings are suggested too and can be changed in SteamVR.

## Settings

In the `[vr]` section of `defaults.ini` (copy them to `options.ini` to override). The main ones:

| Key | Default | What it does |
|---|---|---|
| `enabled` | 0 | 1 = start in VR (or launch with `-vr`; `-novr` turns it off) |
| `renderer` | d3d11 | `dxvk` = run on DXVK (Direct3D 9 on Vulkan, `dxvk\d3d9.dll`), same as `-dxvk` |
| `world scale` | 0.55 | Blitz units per real metre (lower = you feel bigger) |
| `resolution scale` | 1.0 | multiplies the headset's recommended per-eye resolution |
| `snap turn`, `snap angle`, `smooth turn speed` | 1, 30, 2.5 | turning |
| `real crouch`, `crouch height` | 1, 0.45 | crouch in game when your head drops this many metres |
| `show hands`, `hand fullbright` | 1, 0 | hand models; fullbright if they look too dark |
| `hand offset x/y/z`, `hand tilt`, `hand pitch/yaw/roll` | 0, 0, -15/0/0 | adjust where the hands sit on the controllers |
| `hand index touch curl`, `hand thumb touch curl` | 0.3, 0.5 | finger curl when resting on the controls |
| `wrist hud`, `wrist hud offset x/y/z`, `wrist hud size` | 1, 0, 1.0 | meters on the back of the left hand |
| `physical grab`, `grab radius` | 1, 0.12 | grab levers and SCP-914 parts (metres) |
| `physical keycards`, `held item flip` | 1, 0 | keycards / severed hands held in the right hand |
| `documents in hand`, `document size` | 1, 1.0 | documents on a sheet in the left hand |

## Files in this fork

- `VR.bb`: the game's VR module; small hooks in `Main.bb`, `MapSystem.bb`, `UpdateEvents.bb`
- `Userlibs\BlitzXR.decls`, `BlitzXR.dll`, `openxr_loader.dll`: the OpenXR bridge and the Khronos loader (prebuilt, 32-bit)
- `dxvk\d3d9.dll`: DXVK 3.1.1 (32-bit), used with `renderer = dxvk`
- `GFX\VR\`: hand models and texture; `GFX\shaders\VRHudKey.fx`: desktop HUD shader
- `VR\blitzxr\`: source of `BlitzXR.dll`
- `VR\engine\blitzx3d-vr.patch`: the engine changes the game needs
- `VR\licenses\`: third-party licenses

## Building

1. **Engine.** Clone BlitzX3D (https://github.com/Team-X3D/BlitzX3D), check out commit `6b1a0c4`, apply the patch
   with `git am VR\engine\blitzx3d-vr.patch`, and build it in Visual Studio (Release) as its README describes. The patch
   adds `CameraProjOffAxis`, `SetRenderWorldBuffer`, `GraphicsD3D9Device`, `BufferD3D9Surface`, a `-icon` option for
   `blitzcc`, loading DXVK from a `dxvk` folder, and visible runtime errors in release builds. BlitzX3D does not accept
   AI-written contributions, so keep these changes in your own copy and do not send them upstream.
2. **Userlibs.** Copy every `.decls` file from `Userlibs\` (including `BlitzXR.decls`) into the patched BlitzX3D's
   `_release\userlibs` folder.
3. **Game.** From the game folder: `blitzcc -o "SCP - Containment Breach VR.exe" -icon logo.ico Main.bb` (with
   `BLITZPATH` set to the patched BlitzX3D `_release` folder). `BlitzXR.dll` and `openxr_loader.dll` must be next to the
   exe; the game won't start without `BlitzXR.dll`, even with VR off.
4. **BlitzXR (optional, the DLL is included).** It must be 32-bit:
   `cmake -S VR\blitzxr -B build -A Win32` then `cmake --build build --config Release`.

## Licensing

- Game and game changes: CC BY-SA 3.0, like the game
- BlitzXR (`VR\blitzxr`): MIT (`VR\blitzxr\LICENSE`); OpenXR / Vulkan headers: Apache-2.0 / MIT (Khronos)
- OpenXR loader (`openxr_loader.dll`): Apache-2.0 (`VR\licenses\openxr_loader-Apache-2.0.txt`)
- DXVK (`dxvk\d3d9.dll`): zlib (`VR\licenses\DXVK-zlib.txt`)
- Engine patch: AGPL-3.0 for BlitzX3D's code, zlib for the original Blitz3D files (`VR\licenses\`). If you distribute
  a built game exe, you must also offer the patched engine source; the patch in this folder is that source.
- Hand models: "Free Pack - VR Hands (Rigged)" by PolyOne Studio (@polyone),
  https://sketchfab.com/3d-models/free-pack-vr-hands-rigged-b3b8309e59c748e3956531c8f57dd233, CC BY 4.0, converted to
  .b3d (`VR\licenses\VR-hands-CC-BY-4.0.txt`)

This is a fan project, not affiliated with the SCP - Containment Breach developers, SCP-CB-CPP or BlitzX3D.
