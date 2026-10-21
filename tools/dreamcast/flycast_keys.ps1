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
    [double]$Seconds = 1.0,
    # Down presses the keys and leaves them held, Up lets them go (to hold
    # one key while others come and go); Hold does both.
    [ValidateSet('Hold', 'Down', 'Up')]
    [string]$Action = 'Hold',
    # Only gives the game window the focus (run_flycast.ps1 does it at start).
    [switch]$FocusOnly
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
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    // Windows lets only the window with the focus hand it on: sharing its
    // thread's input for a moment makes this one count as it.
    public static void Focus(IntPtr h) {
        uint p;
        uint fg = GetWindowThreadProcessId(GetForegroundWindow(), out p);
        uint me = GetCurrentThreadId();
        bool attached = fg != 0 && fg != me && AttachThreadInput(me, fg, true);
        BringWindowToTop(h);
        SetForegroundWindow(h);
        if (attached) AttachThreadInput(me, fg, false);
    }
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
$window = [IntPtr]::Zero
for ($i = 0; $i -lt 40 -and $window -eq [IntPtr]::Zero; $i++) {
    $window = [FlycastKeys]::FindGame([uint32]$proc.Id)
    if ($window -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 250 }
}
if ($window -eq [IntPtr]::Zero) {
    throw 'Flycast game window not found'
}
if ([FlycastKeys]::GetForegroundWindow() -ne $window) {
    [FlycastKeys]::Focus($window)
    Start-Sleep -Milliseconds 150
}
if ($FocusOnly) {
    return
}
# Keys go to whatever window has the focus: never send them anywhere but
# the game.
if ([FlycastKeys]::GetForegroundWindow() -ne $window) {
    throw 'Flycast does not have the focus; no keys sent'
}

$extended = @('Up', 'Down', 'Left', 'Right', 'Insert', 'Delete', 'Home', 'End', 'PageUp', 'PageDown')
$codes = foreach ($key in $Keys) {
    $vk = [byte][System.Windows.Forms.Keys]$key
    $scan = [byte][FlycastKeys]::MapVirtualKey($vk, 0)
    $flags = if ($extended -contains $key) { 1 } else { 0 }
    [pscustomobject]@{ Vk = $vk; Scan = $scan; Flags = $flags }
}
if ($Action -ne 'Up') {
    foreach ($c in $codes) {
        [FlycastKeys]::keybd_event($c.Vk, $c.Scan, $c.Flags, [UIntPtr]::Zero)
    }
}
if ($Action -eq 'Hold') {
    Start-Sleep -Milliseconds ([int]($Seconds * 1000))
}
if ($Action -ne 'Down') {
    foreach ($c in $codes) {
        [FlycastKeys]::keybd_event($c.Vk, $c.Scan, $c.Flags -bor 2, [UIntPtr]::Zero)
    }
}
