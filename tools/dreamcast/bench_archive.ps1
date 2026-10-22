# Keeps the benchmark reports a serial log holds (DC_Bench_End: the lines
# between "[bench] === report" and "[bench] === end"), each in a file of its
# own under docs\dreamcast\benchmarks, named by its date, build and game:
#
#   .\tools\dreamcast\bench_archive.ps1 -Log build\dc-serial.txt
#
# run_flycast.ps1 runs it on its log when Flycast closes. A report already
# kept is left as it is.
param(
    [Parameter(Mandatory = $true)][string]$Log
)

if (-not (Test-Path $Log)) {
    return
}
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$dir = Join-Path $root 'docs\dreamcast\benchmarks'

$report = $null
foreach ($raw in Get-Content $Log -Encoding UTF8) {
    # Other threads' output can share a line with the report's.
    $at = $raw.IndexOf('[bench] ')
    if ($at -lt 0) {
        continue
    }
    $line = $raw.Substring($at + 8).TrimEnd()
    if ($line -eq '=== report') {
        $report = New-Object System.Collections.Generic.List[string]
        continue
    }
    if ($null -eq $report) {
        continue
    }
    if ($line -ne '=== end') {
        $report.Add($line)
        continue
    }
    $date = ($report | Where-Object { $_ -like 'date *' } | Select-Object -First 1)
    $build = ($report | Where-Object { $_ -like 'build *' } | Select-Object -First 1)
    $game = ($report | Where-Object { $_ -like 'game *' } | Select-Object -First 1)
    $stamp = if ($date) { ($date -replace '^date ', '') -replace '[-: ]', '' } else { 'nodate' }
    $num = if ($build -match '(\d+)') { $Matches[1] } else { 'x' }
    $name = '{0}_build{1}_{2}.txt' -f $stamp, $num, ($game -replace '^game ', '')
    New-Item -ItemType Directory -Force $dir | Out-Null
    $path = Join-Path $dir $name
    if (-not (Test-Path $path)) {
        $report | Set-Content -Path $path -Encoding UTF8
        "benchmark report kept: docs\dreamcast\benchmarks\$name"
    }
    $report = $null
}
