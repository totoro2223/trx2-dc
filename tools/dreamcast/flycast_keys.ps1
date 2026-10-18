# Holds keys down in the Flycast window, to drive the game under test with
# Flycast's keyboard mapping (arrows are the D-pad by default).
#
#   flycast_keys.ps1 -Keys Up -Seconds 2
#
# Keys are System.Windows.Forms.Keys names; several are held together.
param(
    [string[]]$Keys = @('Up'),
    [double]$Seconds = 1.0
)

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class FlycastKeys {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint mapType);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
}
'@
Add-Type -AssemblyName System.Windows.Forms

$proc = Get-Process flycast -ErrorAction SilentlyContinue |
    Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if ($null -eq $proc) {
    throw 'Flycast is not running'
}
[void][FlycastKeys]::SetForegroundWindow($proc.MainWindowHandle)
Start-Sleep -Milliseconds 150

$extended = @('Up', 'Down', 'Left', 'Right', 'Insert', 'Delete', 'Home', 'End', 'PageUp', 'PageDown')
$codes = foreach ($key in $Keys) {
    $vk = [byte][System.Windows.Forms.Keys]$key
    $scan = [byte][FlycastKeys]::MapVirtualKey($vk, 0)
    $flags = if ($extended -contains $key) { 1 } else { 0 }
    [pscustomobject]@{ Vk = $vk; Scan = $scan; Flags = $flags }
}
foreach ($c in $codes) {
    [FlycastKeys]::keybd_event($c.Vk, $c.Scan, $c.Flags, [UIntPtr]::Zero)
}
Start-Sleep -Milliseconds ([int]($Seconds * 1000))
foreach ($c in $codes) {
    [FlycastKeys]::keybd_event($c.Vk, $c.Scan, $c.Flags -bor 2, [UIntPtr]::Zero)
}
