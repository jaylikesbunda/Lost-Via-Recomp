# Lost: Via Domus recomp - toolchain setup.
# Pins mirror graw360-recomp's tools/setup.ps1 (MIT) so its SDK patches apply.
# Installs into <repo>\.tools (a junction to H:\LostRecomp-heavy\.tools on this machine).
param([switch]$SkipSdkSource)
$ErrorActionPreference = 'Stop'
$repo  = Split-Path -Parent $PSScriptRoot
$tools = Join-Path $repo '.tools'
$dl    = Join-Path $tools 'downloads'
New-Item -ItemType Directory -Force $dl | Out-Null

$items = @(
  @{ Name='rexglue-sdk'; Url='https://github.com/rexglue/rexglue-sdk/releases/download/nightly-20261002-bd833a2a/rexglue-sdk-0.10.0.24-dev.gbd833a2-win-amd64.zip'
     Sha='1e19d7ec9be0d9d7e1a0088a34a33b068d4dea520181e81318528ef3b37627e7'; Dest='rexglue-sdk'; Check='rexglue-sdk/win-amd64/bin/rexglue.exe' },
  @{ Name='llvm'; Url='https://github.com/llvm/llvm-project/releases/download/llvmorg-20.1.8/LLVM-20.1.8-win64.exe'
     Sha='3197846a2b19063687dd56e93e34cd941e3548d907f23a6131571321bdf9fe7b'; Dest='llvm'; Check='llvm/bin/clang++.exe'; Installer=$true },
  @{ Name='cmake'; Url='https://github.com/Kitware/CMake/releases/download/v3.31.8/cmake-3.31.8-windows-x86_64.zip'
     Sha='81aa9964dbabd71fe02e7ec50472fd3ad56138c49944515ece9001efbff8d719'; Dest='cmake'; Check='cmake/cmake-3.31.8-windows-x86_64/bin/cmake.exe' },
  @{ Name='ninja'; Url='https://github.com/ninja-build/ninja/releases/download/v1.13.1/ninja-win.zip'
     Sha='26a40fa8595694dec2fad4911e62d29e10525d2133c9a4230b66397774ae25bf'; Dest='ninja'; Check='ninja/ninja.exe' }
)

foreach ($it in $items) {
  if (Test-Path (Join-Path $tools $it.Check)) { "[skip] $($it.Name) already installed"; continue }
  $file = Join-Path $dl (Split-Path $it.Url -Leaf)
  if (-not (Test-Path $file) -or (Get-FileHash $file -Algorithm SHA256).Hash -ne $it.Sha.ToUpper()) {
    "[get ] $($it.Name)"
    curl.exe -sSL --fail -o $file $it.Url
    if ($LASTEXITCODE) { throw "download failed: $($it.Url)" }
  }
  $h = (Get-FileHash $file -Algorithm SHA256).Hash
  if ($h -ne $it.Sha.ToUpper()) { Remove-Item $file; throw "SHA-256 mismatch for $($it.Name): $h" }
  $dest = Join-Path $tools $it.Dest
  if ($it.Installer) {
    $p = Start-Process -FilePath $file -ArgumentList '/S', "/D=$dest" -Wait -PassThru
    if ($p.ExitCode) { throw "$($it.Name) installer exit $($p.ExitCode)" }
  } else {
    Expand-Archive -Path $file -DestinationPath $dest -Force
  }
  if (-not (Test-Path (Join-Path $tools $it.Check))) { throw "$($it.Name): $($it.Check) missing after install" }
  "[ ok ] $($it.Name)"
}

if ($SkipSdkSource) { return }

# Patched SDK source (for translator fixes GRAW found: vpkd3d128, vexptefp, jump tables...).
$sdk    = Join-Path $repo 'third_party\rexglue-sdk-bd833a2'
$commit = 'bd833a2ab69d305f99ddf57418fc38ffec8af93d'
if (-not (Test-Path (Join-Path $sdk '.git'))) {
  git -c core.longpaths=true clone --no-checkout https://github.com/rexglue/rexglue-sdk.git $sdk
  git -C $sdk config core.longpaths true
  git -C $sdk checkout --quiet --detach $commit
}
git -C $sdk submodule update --init --recursive
"[ ok ] SDK source at $((git -C $sdk rev-parse --short HEAD))"
