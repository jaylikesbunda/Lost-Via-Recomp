# Apply graw360-recomp's SDK patches (MIT/BSD-3, tools/patches) to third_party\rexglue-sdk-bd833a2
# in GRAW's order, then build the patched codegen CLI (rexgluerd.exe) and Xenos GPU plugin.
# Run after tools/setup.ps1.
param([int]$Jobs = [Math]::Max(1, [Environment]::ProcessorCount - 2))
$ErrorActionPreference = 'Stop'
$repo  = Split-Path -Parent $PSScriptRoot
$tools = Join-Path $repo '.tools'
$sdk   = Join-Path $repo 'third_party\rexglue-sdk-bd833a2'
$patchDir = Join-Path $PSScriptRoot 'patches'

$order = @(
  'rexglue-bd833a2-vpkd3d128-alias',
  'rexglue-bd833a2-freed-page-resolve',
  'rexglue-bd833a2-vblank-pacing',
  'rexglue-bd833a2-gpu-skip-draws',
  'pgr4-native/0021-gpu-sample-computed-LOD-pixel-shader-fetches-with-sa',
  'pgr4-native/0022-gpu-add-a-pixel-shader-modification-for-textures-kno',
  'rexglue-bd833a2-implicit-gradients-switch',
  'rexglue-bd833a2-spirv-exp-adjust',
  'rexglue-bd833a2-spirv-implicit-gradients',
  'rexglue-bd833a2-vexptefp-underflow',
  'rexglue-bd833a2-gpu-known-registers',
  'rexglue-bd833a2-gpu-rptr-writeback',
  'rexglue-bd833a2-gpu-cp-stats',
  'rexglue-bd833a2-gpu-skip-draws-fast-regs',
  # lost-recomp's own (docs/FINDINGS.md, "Bink videos"):
  'lost-bd833a2-rasterize-multipass',   # Bink quad arrives with vs_export_mode 7; draw it
  'lost-bd833a2-log-draw-drops',        # --gpu_log_draw_drops: why IssueDraw dropped a draw
  'lost-bd833a2-live-refresh-rate',     # vblank timer follows the video mode live; gpu_vblank_hz overrides its rate
  'lost-bd833a2-d3d-command-counts',    # --gpu_log_d3d_command_counts: D3D12 calls replayed per 5 s
  'lost-bd833a2-async-submission',      # d3d12_async_submission: replay + submit on a worker thread
  'lost-bd833a2-draw-call-trim'         # skip unchanged constant writes; one index buffer view per format
)

$branch = (git -C $sdk branch --show-current)
if ($branch -ne 'lost') {
  if (git -C $sdk status --porcelain) { throw "SDK tree is dirty; refusing to patch" }
  foreach ($name in $order) {
    $src = Join-Path $patchDir "$name.patch"
    $tmp = Join-Path $env:TEMP ("lost-" + (Split-Path $name -Leaf) + ".patch")
    # normalise CRLF -> LF like GRAW's setup
    [IO.File]::WriteAllText($tmp, ([IO.File]::ReadAllText($src) -replace "`r`n", "`n"))
    git -C $sdk apply --whitespace=nowarn $tmp
    if ($LASTEXITCODE) { throw "patch failed: $name" }
    "[patch] $name"
  }
  git -C $sdk -c user.name=lost-recomp -c user.email=lost-recomp@localhost add -A
  git -C $sdk -c user.name=lost-recomp -c user.email=lost-recomp@localhost commit --quiet -m "lost-recomp: graw360 SDK patches"
  git -C $sdk switch --quiet -c lost
  "[ ok ] tree $((git -C $sdk rev-parse 'HEAD^{tree}'))  (GRAW expects 7ae9d8dbf56c5277e03efb0ab7a774102671440e)"
}

# libmspack stores some sources as symlinks (mode 120000); Windows git checks them out as tiny
# text files holding the target path. Replace each with a copy of its target (as GRAW's setup does).
$msp = Join-Path $sdk 'thirdparty\libmspack'
foreach ($line in (git -C $msp ls-files -s)) {
  if ($line -match '^120000 \S+ \d+\t(.+)$') {
    $rel = $Matches[1]; $link = Join-Path $msp $rel
    # Where git made real symlinks (the CI runner can) there is nothing to replace.
    $item = Get-Item -LiteralPath $link -Force
    if ($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { continue }
    if ($item.Length -lt 256) {
      $text = ([IO.File]::ReadAllText($link)).Trim()
      if (!$text -or $text.IndexOfAny([IO.Path]::GetInvalidPathChars()) -ge 0) { continue }
      $target = Join-Path (Split-Path $link -Parent) $text
      if (Test-Path -LiteralPath $target -PathType Leaf) { Copy-Item -LiteralPath $target $link -Force; "[link ] $rel" }
    }
  }
}

# Build inside a VS 2022 DevShell with the pinned LLVM + Ninja first on PATH.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -version '[17.0,18.0)' -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$env:PATH = "$tools\llvm\bin;$tools\ninja;$env:PATH"
$cmake = Join-Path $tools 'cmake\cmake-3.31.8-windows-x86_64\bin\cmake.exe'
Push-Location $sdk
try {
  & $cmake --preset win-amd64 -DREXGLUE_BUILD_TESTS=OFF -DREXGLUE_USE_VULKAN=OFF
  if ($LASTEXITCODE) { throw "SDK configure failed" }
  & $cmake --build --preset win-amd64-relwithdebinfo --parallel $Jobs --target rexglue rexgpu-xenos
  if ($LASTEXITCODE) { throw "SDK build failed" }
} finally { Pop-Location }
Get-ChildItem (Join-Path $sdk 'out\win-amd64\RelWithDebInfo') -Filter 'rex*rd.*' | Select-Object Name, Length
