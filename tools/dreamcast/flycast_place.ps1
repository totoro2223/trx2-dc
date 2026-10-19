# Moves Flycast's game window onto the display whose monitor has the given
# name (as Windows reports it, e.g. "JMGO" or "LG TV") and maximises it there. Does
# nothing when no such display is connected.
#
#   .\tools\dreamcast\flycast_place.ps1 -Monitor "JMGO"
param([string]$Monitor = 'JMGO')

Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class FlyPlace {
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
    public struct DISPLAY_DEVICE {
        public int cb;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)] public string DeviceName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=128)] public string DeviceString;
        public int StateFlags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=128)] public string DeviceID;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=128)] public string DeviceKey;
    }
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern bool EnumDisplayDevices(string dev, uint i, ref DISPLAY_DEVICE dd, uint flags);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int w, int hgt, uint flags);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    public static string MonitorId(string adapter) {
        var d = new DISPLAY_DEVICE(); d.cb = Marshal.SizeOf(d);
        return EnumDisplayDevices(adapter, 0, ref d, 1) ? d.DeviceID : "";
    }
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
[void][FlyPlace]::SetProcessDPIAware()

# The monitor's hardware id (e.g. GSM0001) and instance (UID...), by its name.
$wanted = Get-CimInstance -Namespace root\wmi -ClassName WmiMonitorID -ErrorAction SilentlyContinue |
    Where-Object { (($_.UserFriendlyName | Where-Object { $_ -ne 0 } | ForEach-Object { [char]$_ }) -join '') -eq $Monitor } |
    Select-Object -First 1
if ($null -eq $wanted) {
    "monitor '$Monitor' not connected; window left where it is"
    return
}
$parts = $wanted.InstanceName -split '\\'
$hwid = $parts[1]
$uid = ($parts[2] -split '_')[0] -replace '^.*&', ''

$screen = [System.Windows.Forms.Screen]::AllScreens | Where-Object {
    $id = [FlyPlace]::MonitorId($_.DeviceName)
    $id -like "*$hwid*" -and $id -like "*$uid*"
} | Select-Object -First 1
if ($null -eq $screen) {
    "no display found for '$Monitor'"
    return
}

$proc = Get-Process flycast -ErrorAction Stop | Select-Object -First 1
$window = [IntPtr]::Zero
for ($i = 0; $i -lt 40 -and $window -eq [IntPtr]::Zero; $i++) {
    $window = [FlyPlace]::FindGame([uint32]$proc.Id)
    if ($window -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 250 }
}
if ($window -eq [IntPtr]::Zero) {
    throw 'Flycast game window not found'
}
$b = $screen.Bounds
[void][FlyPlace]::ShowWindow($window, 9)  # restore, so that it can move
[void][FlyPlace]::SetWindowPos($window, [IntPtr]::Zero, $b.X + 40, $b.Y + 40, 800, 600, 0x0004)
[void][FlyPlace]::ShowWindow($window, 3)  # maximise on that display
"Flycast on '$Monitor' ($($screen.DeviceName), $($b.Width)x$($b.Height))"
