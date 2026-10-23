# MXReflexFix

ASI plugin for **MX vs ATV Reflex** (Steam, `MXReflex.exe`) that:

- renders the 3D scene across the whole screen on ultrawide displays (Hor+) instead of pillarboxing,
- keeps the HUD, menus and videos at 16:9, centered (and fixes mouse picking to match),
- unlocks the 60 fps cap (default: your `ScreenRefresh` from `graphics.cfg`) and replaces most of the
  game's busy-wait limiter with a high-resolution sleep.

The game already passes a measured frame time to its simulation, so a higher frame rate gives
smaller steps rather than a faster game. See *Testing physics* below.

## Build (NixOS)

```sh
nix-shell
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw32.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The output is `build/MXReflexFix.asi`. After linking, the build prints the DLL's imports. Only Windows
system DLLs (KERNEL32, USER32, msvcrt, ...) may appear there. If `libmcfgthread`, `libstdc++`,
`libgcc` or `libwinpthread` show up, the static runtime didn't get linked in. Report that, or copy
that DLL from the nix store next to the game.

## Build (GitHub Actions)

`.github/workflows/build.yml` builds the ASI with Ubuntu's MinGW i686 compiler (`-DMINGW_SUFFIX=-posix`).
It also fails if the ASI picks up a MinGW runtime DLL dependency.

- Every push and pull request uploads `MXReflexFix.asi`, `MXReflexFix.ini` and `README.md` as a build artifact.
- Pushing a tag such as `v0.2.0` also creates a GitHub release with `MXReflexFix-0.2.0.zip`.
  The version from the tag appears in `MXReflexFix.log`.

```sh
git tag v0.2.0
git push origin v0.2.0
```

## Install

1. Download [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases)
   **Win32** and put its `dinput8.dll` next to `MXReflex.exe`.
2. Copy `MXReflexFix.asi` and `MXReflexFix.ini` next to `MXReflex.exe`.
3. Set your native resolution in `graphics.cfg` (`ScreenWidth`, `ScreenHeight`, `Fullscreen=1`).
4. Start the game. `MXReflexFix.log` appears next to the `.asi`.

## Config (`MXReflexFix.ini`)

| Section / key | Default | Meaning |
|---|---|---|
| `[Fix Resolution] Enabled` | `true` | Full-screen scene render target. Required by the two fixes below. |
| `[Fix Aspect Ratio] Enabled` | `true` | Hor+ projection at the screen aspect ratio. |
| `[FOV] Multiplier` | `1.0` | Scales the vertical FOV of gameplay cameras. |
| `[Fix HUD] Enabled` | `true` | HUD and menus stay 16:9, centered. |
| `[Fix HUD] MenuBlackBars` | `true` | Black bars beside the 16:9 area in the front-end menus; races stay full width. |
| `[Fix HUD] PauseBlackBars` | `true` | Same black bars while the game is paused (`UIManager+0x288`, the flag behind Lua `IsPaused`). |
| `[Intro Skip] Enabled` | `true` | Skip the startup logo videos. |
| `[Intro Skip] Videos` | `Nordic_Logo_wSound,Rainbow_Logo_wSound,DHLogo` | Videos (base names in `Videos\`) to skip. Every opened video is logged. |
| `[Framerate] Unlock` | `true` | Replace the 60 fps cap. |
| `[Framerate] Cap` | `0` | `0` = ScreenRefresh, `-1` = uncapped, otherwise that fps. |

## How it works

All hooks are safetyhook mid-hooks, found by signature:

| Hook | Game function | Change |
|---|---|---|
| Render Target | `sub_7AD6B0` | Main scene RT = backbuffer size instead of the largest 16:9 box. |
| Aspect Ratio | `Camera::GetProjection` `sub_791720` | 16:9 perspective aspect → screen aspect (vertical FOV, so Hor+). |
| RTT Camera | `sub_6CE960` | Jumbotron render-to-texture cameras are excluded from the aspect change. |
| HUD Camera | `Icon` ctor `sub_7FD8D0` | 1280x720 UI ortho camera widened to `720 × aspect`, so UI stays 16:9 centered. |
| Mouse | `sub_817400` | Cursor → UI mapping uses the centered 16:9 HUD rect. |
| End Frame | `sub_7AE3A0` | Right before `EndScene` (the game presents through the swap chain): while the front-end world is active and the race world isn't, it clears the side strips to black. |
| Intro Skip | `BinkOpen` (binkw32.dll) + video decode `sub_595690` | Handles of listed videos are remembered at open (the game preloads all videos). Before each decoded frame such a video is moved to its last frame, so the game's own VideoDone fires right away. |
| Set Target FPS | `Engine::SetTargetFPS` `sub_84E070` | 60 → configured cap. |
| Frame Limiter | `sub_84EA80` | Sleeps until about 1 ms before the deadline, then lets the game's spin finish. |

## Testing physics

Compare `Cap = 60` against `Cap = 0` (refresh rate) and `Cap = -1` (uncapped):

- the same Free Ride straight: top speed and time to a landmark,
- the same jump: landing distance,
- backflip rotation time,
- an AI race on the same track and difficulty: lap times.

Differences should be within normal run-to-run variation. Hands-on testing at 175 fps found the bike
handling normal. If something is off, send `MXReflexFix.log`
and say which test differed.

## Known things to check

- World-anchored markers (rider names, gate arrows) might sit slightly toward the screen center.
- Screen overlays (mud, water, dust) might cover only the 16:9 area.
- Windowed mode: the game forces a 16:9 window, so the fix only matters in fullscreen.
