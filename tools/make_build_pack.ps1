# Assembles the launcher's build pack: everything a player's PC needs to build the game besides
# the compiler and Microsoft's libraries. It contains no game material.
#   rexglue-sdk/  the prebuilt ReXGlue SDK the game links (from tools/setup.ps1)
#   codegen/      the patched code generator and the runtime it was built with (tools/patch-sdk.ps1)
#   plugin/       the patched Xenos GPU plugin
#   sdk-src/      the patched SDK sources the native renderer compiles
#   llvm/ cmake/ ninja/  the compiler (trimmed LLVM), CMake and Ninja
#   xwin/         fetches Microsoft's C++ library and Windows SDK on the player's PC
#   vcrt/         the VC++ runtime DLLs, copied beside the game
# Run after tools/setup.ps1 and tools/patch-sdk.ps1.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/make_build_pack.ps1 [-Out <dir>]
param([string]$Out)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (!$Out) { $Out = Join-Path $repo 'launcher/src-tauri/resources/pack' }
$tools = Join-Path $repo '.tools'
$sdk = Join-Path $repo 'third_party/rexglue-sdk-bd833a2'
$built = Join-Path $sdk 'out/win-amd64/RelWithDebInfo'

foreach ($need in "$tools/rexglue-sdk/win-amd64/bin/rexglue.exe", "$built/rexgluerd.exe", "$built/rexgpu-xenosrd.dll") {
  if (!(Test-Path $need)) { throw "missing ${need}: run tools/setup.ps1 and tools/patch-sdk.ps1 first" }
}
if ((git -C $sdk branch --show-current) -ne 'lost') { throw 'the SDK tree is not patched: run tools/patch-sdk.ps1' }

if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
New-Item -ItemType Directory -Force $Out | Out-Null

function Copy-Tree($from, $to) {
  New-Item -ItemType Directory -Force (Split-Path $to -Parent) | Out-Null
  Copy-Item -Recurse -Force $from $to
}

Copy-Tree "$tools/rexglue-sdk" "$Out/rexglue-sdk"

New-Item -ItemType Directory -Force "$Out/codegen", "$Out/plugin" | Out-Null
Copy-Item "$built/rexgluerd.exe", "$built/rexruntimerd.dll", "$built/TracyClientrd.dll" "$Out/codegen"
Copy-Item "$built/rexgpu-xenosrd.dll" "$Out/plugin"

# The native renderer compiles these (port/CMakeLists.txt, port/native/lost_shader_xlate.cmake).
foreach ($dir in 'include', 'src', 'thirdparty/dxbc', 'thirdparty/glslang', 'thirdparty/vulkan-headers/include',
                 'thirdparty/renderdoc', 'thirdparty/spirv-headers/include') {
  Copy-Tree (Join-Path $sdk $dir) (Join-Path "$Out/sdk-src" $dir)
}

# The compiler: only the LLVM tools the build runs, and clang's own headers.
$llvm = "$tools/llvm"
New-Item -ItemType Directory -Force "$Out/llvm/bin", "$Out/llvm/lib/clang" | Out-Null
foreach ($exe in 'clang', 'clang++', 'lld-link', 'llvm-ar', 'llvm-ranlib', 'llvm-lib', 'llvm-rc', 'llvm-mt') {
  Copy-Item "$llvm/bin/$exe.exe" "$Out/llvm/bin"
}
Copy-Item -Recurse "$llvm/lib/clang/*" "$Out/llvm/lib/clang"
Copy-Item "$llvm/LICENSE.TXT" "$Out/llvm" -ErrorAction SilentlyContinue

Copy-Tree "$tools/cmake/cmake-3.31.8-windows-x86_64" "$Out/cmake"
New-Item -ItemType Directory -Force "$Out/ninja" | Out-Null
Copy-Item "$tools/ninja/ninja.exe" "$Out/ninja"

# xwin downloads Microsoft's C++ library and Windows SDK on the player's PC (they may not be
# redistributed), so no Visual Studio is needed there.
$xwinUrl = 'https://github.com/Jake-Shadle/xwin/releases/download/0.10.0/xwin-0.10.0-x86_64-pc-windows-msvc.tar.gz'
$xwinSha = '96e83665ef5c1406aabd0be603a8d08e67e604f36fda630b6807ee948a614e26'
$dl = Join-Path $tools 'downloads'
New-Item -ItemType Directory -Force $dl | Out-Null
$tgz = Join-Path $dl (Split-Path $xwinUrl -Leaf)
if (!(Test-Path $tgz) -or (Get-FileHash $tgz -Algorithm SHA256).Hash -ne $xwinSha.ToUpper()) {
  curl.exe -sSL --fail -o $tgz $xwinUrl
  if ($LASTEXITCODE) { throw "download failed: $xwinUrl" }
  if ((Get-FileHash $tgz -Algorithm SHA256).Hash -ne $xwinSha.ToUpper()) { Remove-Item $tgz; throw 'xwin SHA-256 mismatch' }
}
$xt = Join-Path $env:TEMP 'lost-xwin'
if (Test-Path $xt) { Remove-Item -Recurse -Force $xt }
New-Item -ItemType Directory -Force $xt, "$Out/xwin" | Out-Null
tar -xzf $tgz -C $xt
Get-ChildItem $xt -Recurse -File | Where-Object Name -in 'xwin.exe', 'LICENSE-MIT', 'LICENSE-APACHE' | Copy-Item -Destination "$Out/xwin"
if (!(Test-Path "$Out/xwin/xwin.exe")) { throw 'xwin.exe missing from its archive' }

# The VC++ runtime DLLs, placed beside the code generator and the game, so players need not
# install the VC++ redistributable (Microsoft allows this app-local deployment).
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$crt = Get-ChildItem "$vs\VC\Redist\MSVC\*\x64\Microsoft.VC14*.CRT" -Directory | Sort-Object FullName | Select-Object -Last 1
if (!$crt) { throw 'the VC++ runtime (VC\Redist\MSVC) was not found' }
New-Item -ItemType Directory -Force "$Out/vcrt" | Out-Null
Copy-Item "$($crt.FullName)\*.dll" "$Out/vcrt"
Copy-Item "$($crt.FullName)\*.dll" "$Out/codegen"

$size = (Get-ChildItem $Out -Recurse -File | Measure-Object Length -Sum).Sum
"build pack: $Out ({0:N0} MB)" -f ($size / 1MB)
