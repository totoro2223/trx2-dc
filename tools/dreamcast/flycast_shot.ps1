# Saves a screenshot of Flycast's game window (not its serial console).
#   .\tools\dreamcast\flycast_shot.ps1 -Out build\shot.png
param([string]$Out = 'build\flycast.png')
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class FlyShot {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
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
$p = Get-Process flycast -ErrorAction Stop | Select-Object -First 1
$h = [FlyShot]::Find([uint32]$p.Id)
if ($h -eq [IntPtr]::Zero) { throw 'Flycast game window not found' }
$r = New-Object FlyShot+RECT
[FlyShot]::GetClientRect($h, [ref]$r) | Out-Null
$w = [Math]::Max(1, $r.R - $r.L); $hgt = [Math]::Max(1, $r.B - $r.T)
$bmp = New-Object System.Drawing.Bitmap $w, $hgt
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
# 3 = PW_CLIENTONLY | PW_RENDERFULLCONTENT (captures GPU-rendered content)
[FlyShot]::PrintWindow($h, $hdc, 3) | Out-Null
$g.ReleaseHdc($hdc)
$outPath = Join-Path (Get-Location) $Out
$bmp.Save($outPath)
"saved $Out (${w}x${hgt})"
