# Boots the most recently built CDI (anywhere under build\) of one game in
# Flycast, saving the serial console to build\dc-serial.txt as
# run_flycast.ps1 does. TR3 discs are the ones named *tr3*.cdi
# (make_disc.sh with TRX_DC_GAME=tr3); every other disc is TR2's.
#
#   .\tools\dreamcast\play_latest.ps1 [-Game tr2|tr3]
param(
    [ValidateSet('tr2', 'tr3')][string]$Game = 'tr2',
    [string]$Monitor = 'JMGO'
)

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
Set-Location $root
$cdi = Get-ChildItem -Path (Join-Path $root 'build') -Filter *.cdi -Recurse |
    Where-Object { ($_.Name -like '*tr3*') -eq ($Game -eq 'tr3') } |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($null -eq $cdi) {
    Add-Type -AssemblyName PresentationFramework
    [System.Windows.MessageBox]::Show(
        "No $($Game.ToUpper()) CDI found under build\.", 'TRX') | Out-Null
    exit 1
}
& (Join-Path $PSScriptRoot 'run_flycast.ps1') -Cdi $cdi.FullName -Seconds 1 `
    -KeepRunning -Monitor $Monitor
