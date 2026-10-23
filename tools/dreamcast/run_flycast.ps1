# Boots a CDI in Flycast and saves the Dreamcast serial console to a file.
# Flycast must have Debug.SerialConsoleEnabled = yes in emu.cfg; it then shows
# the serial output in a console window, which this script reads.
#
#   .\tools\dreamcast\run_flycast.ps1 -Cdi build\dc-debugoptimized\TRX.cdi -Seconds 30
#
# -Hidden runs Flycast on a desktop of its own that is never shown, so that
# it neither takes the focus nor covers the screen; only the serial log is of
# use then (no keys, no screenshots). The [frame] times are the Dreamcast's
# own, so they hold even if Windows runs the hidden game slower.
param(
    [string]$Cdi = 'build\dc-debugoptimized\TRX.cdi',
    [int]$Seconds = 30,
    [string]$Flycast = $(if ($env:TRX_FLYCAST) { $env:TRX_FLYCAST } else { 'G:\Flycast\flycast.exe' }),
    [string]$Out = 'build\dc-serial.txt',
    [switch]$KeepRunning,
    [switch]$Hidden,
    # The display to show the game on, by monitor name; empty leaves it
    # where Flycast puts it.
    [string]$Monitor = 'JMGO',
    # Extra Flycast options, e.g. '-config config:rend.vsync=no'.
    [string]$FlycastArgs = '',
    # Stops as soon as the serial log matches this regular expression (an
    # error, say), instead of waiting out -Seconds.
    [string]$StopOn = ''
)

# Runs in a separate process: attaching to another console is only possible
# outside this one. It keeps reading the console until Flycast is gone, so
# that whatever was printed just before a crash is kept.
$reader = @'
param([int]$ProcessId, [string]$OutPath)
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class DcConRead {
  [DllImport("kernel32.dll")] public static extern bool FreeConsole();
  [DllImport("kernel32.dll")] public static extern bool AttachConsole(uint pid);
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
  public static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr sa, uint disp, uint flags, IntPtr tmpl);
  [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
  [StructLayout(LayoutKind.Sequential)] public struct COORD { public short X, Y; }
  [StructLayout(LayoutKind.Sequential)] public struct SMALL_RECT { public short L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct CSBI { public COORD Size, Cursor; public ushort Attr; public SMALL_RECT Win; public COORD Max; }
  [DllImport("kernel32.dll")] public static extern bool GetConsoleScreenBufferInfo(IntPtr h, out CSBI info);
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode)]
  public static extern bool ReadConsoleOutputCharacterW(IntPtr h, StringBuilder sb, uint len, COORD at, out uint read);
  public static string Dump(uint pid) {
    FreeConsole();
    if (!AttachConsole(pid)) return null;
    IntPtr h = CreateFileW("CONOUT$", 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
    CSBI i; GetConsoleScreenBufferInfo(h, out i);
    var all = new StringBuilder();
    for (short y = 0; y <= i.Cursor.Y; y++) {
      var sb = new StringBuilder(i.Size.X);
      uint r; ReadConsoleOutputCharacterW(h, sb, (uint)i.Size.X, new COORD { X = 0, Y = y }, out r);
      string line = sb.ToString();
      if (line.Length > 120) line = line.Substring(0, 120);
      all.AppendLine(line.TrimEnd());
    }
    CloseHandle(h);
    FreeConsole();
    return all.ToString();
  }
}
"@
while (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue) {
    try {
        $text = [DcConRead]::Dump([uint32]$ProcessId)
        if ($text) { [IO.File]::WriteAllText($OutPath, $text) }
    } catch {
        [IO.File]::AppendAllText("$OutPath.err", "$_`r`n")
    }
    [System.Threading.Thread]::Sleep(300)
}
'@

Get-Process flycast -ErrorAction SilentlyContinue | Stop-Process -Force
$cdiPath = (Resolve-Path $Cdi).Path
$outPath = Join-Path (Get-Location) $Out
$readerPath = Join-Path $env:TEMP 'trx-dc-console-reader.ps1'
Set-Content -Path $readerPath -Value $reader -Encoding UTF8

$logStart = Get-Date
if ($Hidden) {
    Add-Type @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
public static class HiddenDesk {
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
  struct STARTUPINFO {
    public int cb; public string lpReserved, lpDesktop, lpTitle;
    public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
    public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
  }
  [StructLayout(LayoutKind.Sequential)]
  struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }
  [DllImport("user32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
  static extern IntPtr CreateDesktop(string name, IntPtr dev, IntPtr mode, uint flags, uint access, IntPtr sa);
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
  static extern bool CreateProcess(string app, string cmd, IntPtr pa, IntPtr ta, bool inherit, uint flags,
    IntPtr env, string dir, ref STARTUPINFO si, out PROCESS_INFORMATION pi);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
  // The desktop handle stays open: the desktop lives while this script or
  // the game uses it.
  public static int Start(string desktop, string app, string args, string dir) {
    if (CreateDesktop(desktop, IntPtr.Zero, IntPtr.Zero, 0, 0x10000000, IntPtr.Zero) == IntPtr.Zero)
      throw new Win32Exception();
    var si = new STARTUPINFO(); si.cb = Marshal.SizeOf(si); si.lpDesktop = desktop;
    PROCESS_INFORMATION pi;
    if (!CreateProcess(app, "\"" + app + "\" " + args, IntPtr.Zero, IntPtr.Zero, false, 0,
        IntPtr.Zero, dir, ref si, out pi))
      throw new Win32Exception();
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return pi.dwProcessId;
  }
}
'@
    # A hidden run is a silent one: the emulator's volume at 0.
    if ($FlycastArgs -notmatch 'audio:volume') {
        $FlycastArgs = "$FlycastArgs -config audio:volume=0".Trim()
    }
    $procId = [HiddenDesk]::Start('TRX-DC', $Flycast, "$FlycastArgs `"$cdiPath`"", (Split-Path $Flycast))
    $proc = Get-Process -Id $procId
} else {
    $proc = Start-Process $Flycast -ArgumentList "`"$cdiPath`"" `
        -WorkingDirectory (Split-Path $Flycast) -PassThru
}
Start-Sleep -Seconds 2
$readerProc = Start-Process powershell -WindowStyle Hidden -PassThru -ArgumentList `
    "-NoProfile -ExecutionPolicy Bypass -File `"$readerPath`" -ProcessId $($proc.Id) -OutPath `"$outPath`""
if ($Hidden) {
    "Flycast running on the hidden desktop TRX-DC"
} elseif ($Monitor -ne '') {
    try {
        & (Join-Path $PSScriptRoot 'flycast_place.ps1') -Monitor $Monitor
    } catch {
        "could not move Flycast: $_"
    }
}
# The serial console opens last and takes the focus; the game wants it.
if (-not $Hidden) {
    try {
        & (Join-Path $PSScriptRoot 'flycast_keys.ps1') -FocusOnly
    } catch {
        "could not focus Flycast: $_"
    }
}

$deadline = $logStart.AddSeconds($Seconds)
while (-not $proc.HasExited -and (Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 500
    if ($StopOn -ne '' -and (Test-Path $outPath)) {
        $text = try { [IO.File]::ReadAllText($outPath) } catch { '' }
        if ($text -match $StopOn) {
            "Stopped: the log matches '$StopOn'"
            Start-Sleep -Milliseconds 700
            break
        }
    }
}
if ($proc.HasExited) {
    "Flycast exited (code $($proc.ExitCode))"
} elseif (-not $KeepRunning) {
    Stop-Process -Id $proc.Id -Force
}
$readerProc.WaitForExit(5000) | Out-Null
"Serial output written to $Out"
# The benchmarks run in it, kept dated.
& (Join-Path $PSScriptRoot 'bench_archive.ps1') -Log $outPath

$log = Join-Path (Split-Path $Flycast) 'flycast.log'
if ((Test-Path $log) -and (Get-Item $log).LastWriteTime -ge $logStart) {
    Get-Content $log | Select-String 'Fatal|exception|stopped' |
        Select-Object -Last 1 | ForEach-Object { "flycast: $($_.Line)" }
}
