# Holds keys down in the Flycast game window, to drive the game under test
# with Flycast's keyboard mapping (arrows are the D-pad by default).
#
#   flycast_keys.ps1 -Keys Up -Seconds 2
#
# Keys are System.Windows.Forms.Keys names; several are held together. The
# keys must not reach the serial console window: a key there can start a
# console selection, which freezes Flycast's serial output.
param(
    [string[]]$Keys = @('Up'),
    [double]$Seconds = 1.0
)

Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class FlycastKeys {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint mapType);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    public static IntPtr FindGame(uint pid) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, l) => {
            uint p; GetWindowThreadProcessId(h, out p);
            if (p != pid || !IsWindowVisible(h)) return true;
            var sb = new StringBuilder(256); GetWindowText(h, sb, 256);
            string t = sb.ToString();
            if (t.StartsWith("Flycast") && !t.Contains("Serial")) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
'@
Add-Type -AssemblyName System.Windows.Forms

$proc = Get-Process flycast -ErrorAction Stop | Select-Object -First 1
$window = [FlycastKeys]::FindGame([uint32]$proc.Id)
if ($window -eq [IntPtr]::Zero) {
    throw 'Flycast game window not found'
}
[void][FlycastKeys]::SetForegroundWindow($window)
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
