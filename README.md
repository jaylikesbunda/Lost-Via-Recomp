# Lost: Via Domus — Xbox 360 Recompiled

Play the Xbox 360 version of Lost: Via Domus on Windows. This work-in-progress port uses [ReXGlue](https://github.com/rexglue/rexglue-sdk) to statically recompile the game's code. It does not include the game or its data.

## Requirements

- Windows 10 or 11, a 64-bit x86-64-v3 CPU, and a Direct3D 12 capable GPU.
- Your own copy of the Xbox 360 game, extracted to a folder. The supported USA/Europe executable has title ID `5553080F` and SHA-256 `443200592190486539885ECEBDC65D2F8DD7200F3B587EE35D70CD8A4A5B256C`.
- A build of this project. See [BUILDING.md](BUILDING.md) to build from source.

## Setup and play

Place the extracted game files, including `default.xex`, `Yeti.big`, and the video folders, in a `game` folder next to `lost.exe`. Run `lost.exe`. Saves and shader caches default to `Documents\lost`; settings are saved to `lost.toml` beside the executable.

The opening sequence, menus and gameplay have been tested, but a complete playthrough has not yet been verified. Higher frame rates are experimental. The default renderer uses the emulated Xenos graphics path. The native renderer is experimental and can be enabled with `--lost_native --lost_native_primary --lost_target_fps=120` on the command line.

## Controls and settings

Use a controller or keyboard for the game. In the game's Options page, select the added frame rate, resolution, anti-aliasing, sharpening or texture filter row and press Y or left/right to change it. Some graphics settings require a restart. The F1 panel provides further settings.

| Key | Action |
| --- | --- |
| F1 | Settings panel |
| F2 | Toggle performance HUD |
| F3 / F4 / ` | SDK overlays and console |

If the game crashes, reports are written under `logs` next to the executable; attach the crash report when reporting a problem.

## Credits and license

Made possible by [ReXGlue](https://github.com/rexglue/rexglue-sdk), [Xenia](https://github.com/xenia-project/xenia), [GRAW 360 Recompiled](https://github.com/Juansolo457/graw360-recomp), and [pgr4-native](https://github.com/shaunsingh/pgr4-native). See [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt) for their licenses. Original project code is available under the [MIT License](LICENSE).

Lost: Via Domus is © Ubisoft Entertainment and ABC Studios. This independent project is not affiliated with or endorsed by Ubisoft, ABC, or Disney. You must supply your own game files.
