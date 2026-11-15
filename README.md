<div align="center">
  <h1>Lost: Via Domus</h1>
  <p>A static recompilation of the Xbox 360 version into a native Windows game.</p>
  <p>
    <a href="LICENSE"><img src="https://img.shields.io/badge/Licence-MIT-555?style=for-the-badge" alt="Licence: MIT"></a>
  </p>
</div>

> [!IMPORTANT]
> The game is not included. You need your own extracted copy of the Xbox 360 game **Lost: Via Domus**.
> No game code or game files are distributed.

The game's PowerPC code is translated to C++ with [ReXGlue](https://github.com/rexglue/rexglue-sdk). This is a work in progress.

| Release | Title ID | Status |
| --- | --- | --- |
| USA / Europe | `5553080F` | Supported |
| Other regions and revisions | | Not supported |

The supported `default.xex` has SHA-256 `443200592190486539885ECEBDC65D2F8DD7200F3B587EE35D70CD8A4A5B256C`.

## Features

- **Frame rate.** 30 (console), 60, 120, 144 or uncapped.
- **Display.** Resolution, anti-aliasing, sharpening and texture filtering, set from the game's Options page.
- **Input.** Controller or keyboard.
- **Native renderer.** An experimental Direct3D 12 renderer, off by default.

## Requirements

| Component | Requirement |
| --- | --- |
| OS | Windows 10 or 11, 64-bit |
| CPU | x86-64-v3, including AVX2 and FMA |
| GPU | Direct3D 12 |

## Quick start

1. Build `lost.exe` (see below).
2. Put your extracted game files, including `default.xex`, `Yeti.big` and the video folders, in a `game` folder next to `lost.exe`.
3. Run `lost.exe`.

Saves and shader caches are stored in `Documents\lost`. Settings are saved to `lost.toml` next to the executable.

| Key | Action |
| --- | --- |
| F1 | Settings panel |
| F2 | Performance HUD |

To try the native renderer, start the game with `--lost_native_primary --lost_target_fps=120`.

## Known limitations

A full playthrough has not been verified yet. Frame rates above 30 and the native renderer are experimental. Some graphics settings need a restart.

If the game crashes, a report is written to `logs` next to the executable. Please attach it when reporting a problem.

<details>
<summary><b>Building from source</b></summary>

See [BUILDING.md](BUILDING.md).

</details>

## Credits and legal

Built on [ReXGlue](https://github.com/rexglue/rexglue-sdk) and [Xenia](https://github.com/xenia-project/xenia), with work adapted from [GRAW 360 Recompiled](https://github.com/Juansolo457/graw360-recomp) and [pgr4-native](https://github.com/shaunsingh/pgr4-native). Third-party licences are in [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt).

Unofficial fan project, not affiliated with or endorsed by Ubisoft, ABC Studios or Disney. Lost: Via Domus and related names belong to their owners. Project code is under the [MIT License](LICENSE), which does not cover game code, game data or generated code.
