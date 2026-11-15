# Native renderer triage: one run in observe mode (Xenos in the game window, native beside it),
# a paired capture (native + Xenos PNG + PSNR in captures.csv) at each of the given seconds.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/native_triage.ps1 [-At '12,25,60,120,190,205'] [-Out H:\LostRecomp-heavy\triage]
# Default timings follow tools/autopilot/speed.txt: boot logos, main menu, loading, the opening
# cutscene, jungle gameplay. Kills only the process it started.
param([string]$At = '12,20,26,45,90,140,180,195,208', [string]$Out = 'H:\LostRecomp-heavy\triage',
      [string]$Extra = '', [string]$UserDir = 'H:\LostRecomp-heavy\user-savetest')
$ErrorActionPreference = 'Stop'
$times = $At -split '[,\s]+' | Where-Object { $_ } | ForEach-Object { [int]$_ } | Sort-Object
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root 'port\out\build\win-amd64-relwithdebinfo\lost.exe'
New-Item -ItemType Directory -Force $Out | Out-Null
Get-ChildItem $Out -Filter *.png -ErrorAction SilentlyContinue | Remove-Item
Remove-Item (Join-Path $Out 'captures.csv') -ErrorAction SilentlyContinue
$log = Join-Path $Out ("triage-" + (Get-Date -Format 'yyyyMMdd-HHmmss') + ".log")
$autopilot = Join-Path $root 'tools\autopilot\speed.txt'
$args = @('--no-fullscreen', '--resolution', '720p', '--game_data_root', (Join-Path $root 'port\assets'),
          '--user_data_root', $UserDir, '--log_file', $log, '--log_level', 'info', '--log_flush_interval', '1',
          '--lost_native', '--lost_target_fps=120', "--lost_native_capture_dir=$Out",
          "--lost_autopilot=$autopilot") + ($Extra -split ' ' | Where-Object { $_ })
$quoted = $args | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }
$p = Start-Process -FilePath $exe -WorkingDirectory (Join-Path $root 'port') -ArgumentList $quoted -PassThru
$elapsed = 0
foreach ($t in $times) {
  if ($p.WaitForExit(($t - $elapsed) * 1000)) { break }
  $elapsed = $t
  New-Item -ItemType File -Force (Join-Path $Out 'capture.request') | Out-Null
  "requested capture at $t s"
}
if (-not $p.WaitForExit(4000)) { Stop-Process -Id $p.Id -Force }
$toml = Join-Path $root 'port\out\build\win-amd64-relwithdebinfo\lost.toml'
Remove-Item $toml -ErrorAction SilentlyContinue
"log: $log"
if (Test-Path (Join-Path $Out 'captures.csv')) { Get-Content (Join-Path $Out 'captures.csv') }
