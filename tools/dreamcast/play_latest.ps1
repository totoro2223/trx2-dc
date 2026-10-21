# Boots the most recently built CDI (anywhere under build\) in Flycast, saving
# the serial console to build\dc-serial.txt as run_flycast.ps1 does.
#
#   .\tools\dreamcast\play_latest.ps1
param([string]$Monitor = 'JMGO')

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
Set-Location $root
$cdi = Get-ChildItem -Path (Join-Path $root 'build') -Filter *.cdi -Recurse |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($null -eq $cdi) {
    Add-Type -AssemblyName PresentationFramework
    [System.Windows.MessageBox]::Show('No CDI found under build\.', 'TRX') | Out-Null
    exit 1
}
& (Join-Path $PSScriptRoot 'run_flycast.ps1') -Cdi $cdi.FullName -Seconds 1 `
    -KeepRunning -Monitor $Monitor
