# Build the Lost: Via Domus port: configure, codegen (patched generator when needed), compile.
# Adapted from graw360-recomp tools/build.ps1 (MIT).
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/build.ps1 [-Configuration RelWithDebInfo] [-Jobs 14]
[CmdletBinding()]
param(
    [ValidateSet('Debug','Release','RelWithDebInfo')][string]$Configuration = 'RelWithDebInfo',
    [ValidateRange(1,64)][int]$Jobs = [Math]::Max(1, [Environment]::ProcessorCount - 2)
)
$ErrorActionPreference = 'Stop'
$root  = Split-Path $PSScriptRoot -Parent
$port  = Join-Path $root 'port'
$tools = Join-Path $root '.tools'
$cmake = "$tools/cmake/cmake-3.31.8-windows-x86_64/bin/cmake.exe"
if (!(Test-Path $cmake)) { throw "Toolchain missing at ${tools}: run tools/setup.ps1." }

$xex = Join-Path $port 'assets/default.xex'
$expected = '443200592190486539885ece bdc65d2f8dd7200f3b587ee35d70cd8a4a5b256c' -replace ' ', ''
if (!(Test-Path $xex)) { throw 'port/assets/default.xex is missing: extract the disc first (tools/xdvdfs.py).' }
if ((Get-FileHash $xex -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) {
    throw 'port/assets/default.xex is not the supported Lost: Via Domus executable (title 5553080F).'
}

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -version '[17.0,18.0)' -property installationPath
Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$env:PATH = "$tools/llvm/bin;$tools/ninja;$env:PATH"

$preset = 'win-amd64-' + $Configuration.ToLowerInvariant()
$prefix = "-DCMAKE_PREFIX_PATH=$tools/rexglue-sdk/win-amd64"
$flags  = '-march=x86-64-v3'
$build  = Join-Path $port "out/build/$preset"
$archFlags = @("-DCMAKE_C_FLAGS=$flags", "-DCMAKE_CXX_FLAGS=$flags", '-B', $build)

Push-Location $port
try {
    & $cmake --preset $preset $prefix @archFlags
    if ($LASTEXITCODE) { throw 'CMake configure failed.' }
    $firstTime = !(Test-Path (Join-Path $port 'generated/default/sources.cmake'))
    & $cmake --build $build --parallel $Jobs --target lost_codegen
    if ($LASTEXITCODE) { throw 'Code generation failed.' }

    $generated = Join-Path $port 'generated/default'
    $checker = Join-Path $PSScriptRoot 'check_vector_aliasing.py'
    & python -I $checker $generated | Out-Null
    if ($LASTEXITCODE) {
        $patched = Join-Path $root 'third_party/rexglue-sdk-bd833a2/out/win-amd64/RelWithDebInfo/rexgluerd.exe'
        if (!(Test-Path $patched)) { throw 'Generated code needs the patched generator (vpkd3d128/vexptefp): run tools/patch-sdk.ps1.' }
        Write-Host 'Regenerating with the patched code generator (vpkd3d128 aliasing, vexptefp underflow)'
        # The source-built generator reports 0.10.1 and rewrites generated/rexglue.cmake to
        # require that SDK version; we link the prebuilt 0.10.0 runtime, so keep the original.
        $glue = Join-Path $port 'generated/rexglue.cmake'
        $glueText = [IO.File]::ReadAllText($glue)
        & $patched codegen --ignore-stamp (Join-Path $port 'lost_manifest.toml')
        $ec = $LASTEXITCODE
        [IO.File]::WriteAllText($glue, $glueText)
        if ($ec) { throw 'Patched code generation failed.' }
        & python -I $checker $generated
        if ($LASTEXITCODE) { throw 'Patched generator still left vector hazards.' }
    }
    if ($firstTime) {
        & $cmake --preset $preset $prefix @archFlags
        if ($LASTEXITCODE) { throw 'CMake re-configure failed.' }
    }
    & $cmake --build $build --parallel $Jobs
    if ($LASTEXITCODE) { throw 'Build failed.' }

    # The patched Xenos GPU plugin (freed-page resolve, vblank pacing, rptr writeback...).
    $suffix = @{ Debug = 'd'; Release = ''; RelWithDebInfo = 'rd' }[$Configuration]
    $plugin = Join-Path $root "third_party/rexglue-sdk-bd833a2/out/win-amd64/$Configuration/rexgpu-xenos$suffix.dll"
    if (Test-Path $plugin) { Copy-Item $plugin $build -Force; "copied patched $(Split-Path $plugin -Leaf)" }
    else { Write-Warning "patched GPU plugin not found ($plugin); using the prebuilt SDK one" }
} finally { Pop-Location }
Get-ChildItem $build -Filter 'lost*.exe' | Select-Object Name, Length
