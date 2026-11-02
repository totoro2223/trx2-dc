# Plays every level of each game in turn on test discs, in a hidden and
# silent Flycast, and writes a report: whether each game reached the end
# ("[levels] done") without an assertion or exception, and its memory figures
# (level region, load peak, what each level is made of).
#
#   .\tools\dreamcast\test_levels.ps1                      # tr1, tr2, tr3
#   .\tools\dreamcast\test_levels.ps1 -Games tr2 -Tag fix  # one game
#
# Builds nothing: run build.sh first (update_base.ps1 does). Exits 1 if a
# game failed. Data paths default to this PC's; pass them, or set
# TRX_DC_DATA_TR1/2/3, on another.
param(
    [string[]]$Games = @('tr1', 'tr2', 'tr3'),
    [string]$BuildDir = 'build/dc-minsize',
    [string]$Tag = (Get-Date -Format 'yyyyMMdd-HHmm'),
    [string]$DataTR1 = $(if ($env:TRX_DC_DATA_TR1) { $env:TRX_DC_DATA_TR1 } else { 'D:\trx\trx2-dc\build\tr1-classic\DATA' }),
    [string]$DataTR2 = $(if ($env:TRX_DC_DATA_TR2) { $env:TRX_DC_DATA_TR2 } else { 'D:\Program Files (x86)\Steam\steamapps\common\Tomb Raider (II)\data' }),
    [string]$DataTR3 = $(if ($env:TRX_DC_DATA_TR3) { $env:TRX_DC_DATA_TR3 } else { 'D:\Program Files (x86)\Steam\steamapps\common\TombRaider (III)\data' }),
    # A copy of Flycast for tests, never the player's own (TRASPASO.md).
    [string]$Flycast = $(if ($env:TRX_FLYCAST) { $env:TRX_FLYCAST } else { 'D:\FlycastTest\flycast.exe' }),
    # A sound output nobody listens to, so that a hidden run is silent.
    [string]$AudioDevice = 'Realtek Digital Output(Optical) (Realtek High Definition Audio)',
    [int]$FramesPerLevel = 300,
    [int]$Seconds = 1500
)

$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot)
Set-Location $root
if (-not $env:DREAMSDK_HOME) { $env:DREAMSDK_HOME = 'D:\DreamSDK' }

# The last level of each game, as TRX_DC_LEVELS counts them.
$lastLevel = @{ tr1 = 15; tr2 = 17; tr3 = 19 }
$data = @{ tr1 = $DataTR1; tr2 = $DataTR2; tr3 = $DataTR3 }
$logDir = "build/logs/test-$Tag"
New-Item -ItemType Directory -Force $logDir | Out-Null
$failed = @()
$rows = @()

foreach ($g in $Games) {
    $dataUnix = $data[$g] -replace '\\', '/'
    if ($dataUnix -match '^([A-Za-z]):(.*)$') {
        $dataUnix = '/' + $Matches[1].ToLower() + $Matches[2]
    }
    $cdi = "$BuildDir/T-test-$g.cdi"
    Remove-Item $cdi -ErrorAction SilentlyContinue
    $vars = "TRX_DC_GAME=$g TRX_DC_LEVELS='$($lastLevel[$g]) $FramesPerLevel' TRX_DC_FMV=0 TRX_DC_MUSIC=0"
    if ($g -eq 'tr1') { $vars = "$vars TRX_DC_LEGAL='$dataUnix/EIDOSPC.PCX'" }
    "== ${g}: disc"
    & "$PSScriptRoot\dcsh.ps1" "$vars bash tools/dreamcast/make_disc.sh $BuildDir '$dataUnix' $cdi 0 2>&1 | tail -1"
    if (-not (Test-Path $cdi)) { $failed += "$g (no disc)"; continue }

    "== ${g}: playing every level (hidden, silent)"
    $log = "$logDir/$g.txt"
    & "$PSScriptRoot\run_flycast.ps1" -Flycast $Flycast -AudioDevice $AudioDevice `
        -Cdi $cdi -Hidden -Seconds $Seconds -Out $log `
        -StopOn 'exception [0-9a-f]{4}|Assertion|aborting|starting the disc over|\[levels\] done' | Out-Null

    $text = Get-Content $log -Raw -ErrorAction SilentlyContinue
    $done = $text -match '\[levels\] done'
    $fault = [regex]::Match($text, 'exception [0-9a-f]{4}[^\r\n]*|Assertion[^\r\n]*|aborting[^\r\n]*|starting the disc over').Value
    $loads = ([regex]::Matches($text, '\[gbuf\] total')).Count
    $region = [regex]::Match($text, 'game heap: (\d+)K').Groups[1].Value
    $peak = ([regex]::Matches($text, 'peak (\d+)K') | ForEach-Object { [int]$_.Groups[1].Value } | Measure-Object -Maximum).Maximum
    $ok = $done -and -not $fault
    if (-not $ok) { $failed += $g }
    $rows += "| $g | $(if ($ok) { 'OK' } else { 'FAILED' }) | $loads | $region | $peak | $fault |"
}

$report = "$logDir/REPORT.md"
@(
    "# Level pass $Tag ($(git rev-parse --short HEAD), $(git branch --show-current))"
    ''
    '| Game | Result | Loads | Region K | Peak K | First fault |'
    '|---|---|---:|---:|---:|---|'
    $rows
    ''
    '## Level data by buffer (K)'
    ''
    (py -3 -I tools/dreamcast/gbuf_summary.py (Get-ChildItem "$logDir/*.txt" | Where-Object Name -ne 'REPORT.md' | ForEach-Object FullName))
) | Set-Content -Encoding utf8 $report
"Report: $report"
if ($failed.Count -gt 0) { "FAILED: $($failed -join ', ')"; exit 1 }
'All games passed.'
