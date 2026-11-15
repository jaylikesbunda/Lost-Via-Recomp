# Play: start lost.exe windowed with debug logging, no time limit, and wait until the player closes
# it. Prints the exit code and the end of the log, and points at any crash report written.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/launch.ps1 [-Fullscreen] [-LogLevel debug] [-Extra '...']
param([switch]$Fullscreen, [switch]$Windowed, [string]$LogLevel = 'debug', [string]$Extra = '')
$ErrorActionPreference = 'Stop'
$root  = Split-Path $PSScriptRoot -Parent
$port  = Join-Path $root 'port'
$exe   = Join-Path $port 'out\build\win-amd64-relwithdebinfo\lost.exe'
$heavy = 'H:\LostRecomp-heavy'
$logs  = Join-Path $heavy 'logs'
New-Item -ItemType Directory -Force $logs | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$log = Join-Path $logs "play-$stamp.log"
# Window mode / resolution come from lost.toml (F1 settings) unless -Fullscreen/-Windowed is given.
$args = @($(if ($Fullscreen) { '--fullscreen' } elseif ($Windowed) { '--no-fullscreen' } else { @() }),
          # Saves + shader cache: the app's defaults (Documents\lost), same as a double-click.
          '--game_data_root', "$port\assets", '--log_file', $log, '--log_level', $LogLevel,
          '--log_flush_interval', '1', '--lost_perf_csv', ($log -replace '\.log$', '-frames.csv')) +
        @([regex]::Matches($Extra, '(?:[^\s"]+|"[^"]*")+') | ForEach-Object { $_.Value -replace '"', '' })
$quoted = $args | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }
$started = Get-Date
$p = Start-Process -FilePath $exe -WorkingDirectory $port -ArgumentList $quoted -PassThru
"started pid $($p.Id), log $log"
$p.WaitForExit()
"exited after {0:N0} s with code {1} (0x{1:X8})" -f ((Get-Date) - $started).TotalSeconds, $p.ExitCode
Get-ChildItem (Join-Path $port 'logs') -Filter 'crash-*.txt' -ErrorAction SilentlyContinue |
  Where-Object LastWriteTime -gt $started | ForEach-Object { "CRASH REPORT: $($_.FullName)" }
& python -I (Join-Path $PSScriptRoot 'perf_report.py') $log
