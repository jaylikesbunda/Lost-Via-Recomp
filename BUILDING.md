# Building from source

Requires Windows 10/11, Visual Studio 2022 with C++ tools, Python 3, Git, and approximately 10 GB of free space. The build targets x86-64-v3. Setup downloads the pinned toolchain and ReXGlue SDK.

```powershell
powershell -ExecutionPolicy Bypass -File tools/setup.ps1
powershell -ExecutionPolicy Bypass -File tools/patch-sdk.ps1
# Extract your own supported Xbox 360 disc into port/assets:
python -I tools/xdvdfs.py extract "<your disc>.iso" port/assets
powershell -ExecutionPolicy Bypass -File tools/build.ps1
```

The supported executable is title `5553080F`, `default.xex` SHA-256 `443200592190486539885ECEBDC65D2F8DD7200F3B587EE35D70CD8A4A5B256C`. The output is `port/out/build/win-amd64-relwithdebinfo/lost.exe`. Run it directly; it finds `port/assets` in this checkout. The build uses the stock SDK runtime and copies the patched Xenos GPU plugin into the output directory.

Game files, generated game code, shader caches and other game-derived material must not be committed. Before committing, stage the intended files and run `python -I tools/audit_repo.py`.

## Native pipeline precompilation

`tools/pipeline_manifest.py` exports shader hashes, sizes, prefix fingerprints and pipeline state from a local native cache. It excludes shader code and driver binaries:

```powershell
python -I tools/pipeline_manifest.py "<cache_root>/lost-native/pipelines.bin" "<output>/pipelines.manifest" --pack port/assets/SHADERS_XEN_Microcode.sdf
```

Place `pipelines.manifest` beside the executable, or pass `--lost_native_pipeline_manifest="<output>/pipelines.manifest"`. Guest shader creation and declaration-patching hooks match shader programs against the manifest. Available pipelines compile on the existing background workers; entries whose shaders are not available remain deferred. A manifest only covers recorded pipelines, and shader specializations generated at draw time may still compile on first use. The original cache remains local game-derived data.

At native startup the loader also reads the supported disc's SDF shader pack and reproduces its boolean-control-flow variants, accepting only exact hashes requested by the manifest. Vertex and pixel object registration supply additional programs. The manifest still describes known pipeline combinations; this does not yet enumerate every possible material/mesh pipeline.

`tools/shader_match.py <sdf> <local-cache> --out <report.json>` matches shaders structurally and verifies control-flow reconstruction. `--inventory` in place of the cache enumerates all disc-program boolean variants. Reports contain metadata only; keep all generated reports outside Git.

Version 2 manifests also describe source program references, boolean values and typed vertex input layouts. The runtime rebuilds the shader from the user's disc and verifies its full expected hash before accepting it. With a manifest present, available pipelines finish compiling before the first native frame by default (`--lost_native_precompile_manifest=false` opts out). This can lengthen startup with an empty driver/pipeline cache. Built-in/runtime-generated shaders and previously unlisted pipeline combinations can still compile on first use.
