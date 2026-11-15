# Verify the production native shader catalog without starting the guest or GPU.
param([Parameter(Mandatory=$true)][string]$Pack,
      [Parameter(Mandatory=$true)][string]$Manifest,
      [string]$OutDir = (Join-Path $PSScriptRoot '..\out\shader-probe'), [int]$Expected = -1,
      [switch]$Bindings)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -version '[17.0,18.0)' -property installationPath
Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$sdk = Join-Path $root '.tools/rexglue-sdk/win-amd64'
$build = Join-Path $root 'port/out/build/win-amd64-relwithdebinfo'
$objects = Join-Path $build 'CMakeFiles/lost_native.dir/native'
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$exe = Join-Path $OutDir 'probe_shader_catalog.exe'
$arguments = @('-std=c++23','-O2','-D_DLL','-D_MT','-Xclang','--dependent-lib=msvcrt','-DSPDLOG_COMPILED_LIB',
 '-DSPDLOG_FMT_EXTERNAL','-DNOMINMAX','-DWIN32_LEAN_AND_MEAN','-I',
 (Join-Path $root 'third_party/rexglue-sdk-bd833a2/include'),'-I',"$sdk/include",'-I',
 (Join-Path $root 'port/native'),(Join-Path $PSScriptRoot 'probe_shader_catalog.cpp'),
 "$objects/pipeline_warmup.cpp.obj","$objects/pipeline_store.cpp.obj","$objects/shader_specialize.cpp.obj",
 "$objects/shader_cache.cpp.obj","$objects/shader_translate.cpp.obj","$build/lost_shader_xlate.lib",
 "$sdk/lib/rexruntimerd.lib","$sdk/lib/fmtrd.lib","$sdk/lib/spdlogrd.lib",
 "$sdk/lib/xxhashrd.lib","$sdk/lib/TracyClientrd.lib",'-ld3d12.lib','-ldxgi.lib','-ldxguid.lib','-Wl,/OPT:REF,/NODEFAULTLIB:libcmt.lib,/NODEFAULTLIB:libucrt.lib','-lucrt.lib','-lvcruntime.lib','-lmsvcrt.lib','-lmsvcprt.lib','-o',$exe)
& (Join-Path $root '.tools/llvm/bin/clang++.exe') @arguments
if ($LASTEXITCODE) { throw 'Catalog probe build failed' }
$env:PATH = "$build;$env:PATH"
$probeArguments = @($Pack,$Manifest)
if ($Expected -ge 0 -or $Bindings) { $probeArguments += [string]$Expected }
if ($Bindings) { $probeArguments += '--bindings' }
& $exe @probeArguments
if ($LASTEXITCODE) { throw 'Production catalog verification failed' }
