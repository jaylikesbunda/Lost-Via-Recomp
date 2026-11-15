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
