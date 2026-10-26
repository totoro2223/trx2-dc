# Saves a screenshot of Flycast's game window (not its serial console).
# The window is brought to the front and copied from the screen, since
# PrintWindow cannot see what Direct3D draws.
#   .\tools\dreamcast\flycast_shot.ps1 -Out build\shot.png
param([string]$Out = 'build\flycast.png', [string]$Path = '')
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class FlyShot {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  public static IntPtr FindConsole(uint pid) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => {
      var sb = new StringBuilder(256); GetWindowText(h, sb, 256);
      if (sb.ToString().Contains("Serial Output")) { found = h; return false; }
      return true;
    }, IntPtr.Zero);
    return found;
  }
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  public static IntPtr Find(uint pid) {
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
"@
[FlyShot]::SetProcessDPIAware() | Out-Null
$p = Get-Process flycast -ErrorAction Stop | Where-Object { $Path -eq '' -or $_.Path -eq $Path } | Select-Object -First 1
$h = [FlyShot]::Find([uint32]$p.Id)
if ($h -eq [IntPtr]::Zero) { throw 'Flycast game window not found' }
# The serial console window would cover the game; it is minimised.
$console = [FlyShot]::FindConsole([uint32]$p.Id)
if ($console -ne [IntPtr]::Zero) { [FlyShot]::ShowWindow($console, 6) | Out-Null }
[FlyShot]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 400
# The copy is of the screen: with another window in front, it would show
# that window instead of the game.
if ([FlyShot]::GetForegroundWindow() -ne $h) {
    throw 'Flycast is not in front; no screenshot taken'
}
$r = New-Object FlyShot+RECT
[FlyShot]::GetClientRect($h, [ref]$r) | Out-Null
$pt = New-Object FlyShot+POINT
[FlyShot]::ClientToScreen($h, [ref]$pt) | Out-Null
$w = [Math]::Max(1, $r.R - $r.L); $hgt = [Math]::Max(1, $r.B - $r.T)
$bmp = New-Object System.Drawing.Bitmap $w, $hgt
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($pt.X, $pt.Y, 0, 0, (New-Object System.Drawing.Size $w, $hgt))
$outPath = Join-Path (Get-Location) $Out
$bmp.Save($outPath)
"saved $Out (${w}x${hgt})"
