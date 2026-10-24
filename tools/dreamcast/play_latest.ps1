# Boots the most recently built CDI (anywhere under build\) of one game in
# Flycast, saving the serial console to build\dc-serial.txt as
# run_flycast.ps1 does. TR1 and TR3 discs are the ones named *tr1*.cdi and
# *tr3*.cdi (make_disc.sh with TRX_DC_GAME=tr1 or tr3); every other disc is
# TR2's; the disc of all three games is the one named *-all*.cdi
# (make_disc_all.sh).
#
#   .\tools\dreamcast\play_latest.ps1 [-Game tr1|tr2|tr3|all]
param(
    [ValidateSet('tr1', 'tr2', 'tr3', 'all')][string]$Game = 'tr2',
    [string]$Monitor = 'JMGO'
)

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
Set-Location $root
$cdi = Get-ChildItem -Path (Join-Path $root 'build') -Filter *.cdi -Recurse |
    Where-Object {
        $disc = if ($_.Name -like '*-all*') { 'all' }
                elseif ($_.Name -like '*tr1*') { 'tr1' }
                elseif ($_.Name -like '*tr3*') { 'tr3' }
                else { 'tr2' }
        $disc -eq $Game
    } |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($null -eq $cdi) {
    Add-Type -AssemblyName PresentationFramework
    [System.Windows.MessageBox]::Show(
        "No $($Game.ToUpper()) CDI found under build\.", 'TRX') | Out-Null
    exit 1
}
& (Join-Path $PSScriptRoot 'run_flycast.ps1') -Cdi $cdi.FullName -Seconds 1 `
    -KeepRunning -Monitor $Monitor
