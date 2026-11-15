# Boot check: start lost.exe windowed at 720p with debug logging, stop it after N seconds, print the
# end of the log. Its window DOES appear on the user's screen. Only stops the process it started.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/smoke.ps1 [-Seconds 40] [-Extra '--log_level trace']
# -Extra is split on whitespace; pass one argument containing spaces as --name="value with spaces".
# -CaptureAt 20,40 screenshots the game window at those seconds into private/captures.
param([int]$Seconds = 40, [string]$Extra = '', [switch]$NotHeadless, [int[]]$CaptureAt = @(),
      [string]$LogDir = '', [string]$LogLevel = 'debug', [string]$UserDir = '', [string]$CacheDir = '')
$ErrorActionPreference = 'Stop'
$root  = Split-Path $PSScriptRoot -Parent
$port  = Join-Path $root 'port'
$exe   = Join-Path $port 'out\build\win-amd64-relwithdebinfo\lost.exe'
$heavy = 'H:\LostRecomp-heavy'
$user  = if ($UserDir) { $UserDir } else { Join-Path $heavy 'user-smoke' }
$logs  = if ($LogDir) { $LogDir } else { Join-Path $port 'logs' }
New-Item -ItemType Directory -Force $logs, "$heavy\user-smoke" | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$log = Join-Path $logs "smoke-$stamp.log"
$args = @('--no-fullscreen', '--resolution', '720p',
          '--game_data_root', "$port\assets", '--user_data_root', $user,
          '--cache_root', $(if ($CacheDir) { $CacheDir } else { Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'lost\cache' }), '--log_file', $log, '--log_level', $LogLevel,
          '--log_flush_interval', '1') + $(if ($NotHeadless) { @() } else { @('--headless=true') }) +
        @([regex]::Matches($Extra, '(?:[^\s"]+|"[^"]*")+') | ForEach-Object { $_.Value -replace '"', '' })
$quoted = $args | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }
$p = Start-Process -FilePath $exe -WorkingDirectory $port -ArgumentList $quoted -PassThru
$elapsed = 0
foreach ($t in ($CaptureAt | Sort-Object)) {
  if ($t -ge $Seconds) { break }
  if ($p.WaitForExit(($t - $elapsed) * 1000)) { break }
  $elapsed = $t
  try { & (Join-Path $PSScriptRoot 'capture_window.ps1') -ProcessId $p.Id -Out (Join-Path $root 'private\captures') }
  catch { "capture at $t s failed: $_" }
}
$exited = $p.WaitForExit([Math]::Max(0, $Seconds - $elapsed) * 1000)
if (-not $exited) { Stop-Process -Id $p.Id -Force; "stopped after $Seconds s (still running)" }
else { "exited by itself after <= $Seconds s, code $($p.ExitCode)" }
"log: $log"
if (Test-Path $log) { Get-Content $log -Tail 60 }
