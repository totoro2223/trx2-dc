# Boots a CDI in Flycast and saves the Dreamcast serial console to a file.
# Flycast must have Debug.SerialConsoleEnabled = yes in emu.cfg; it then shows
# the serial output in a console window, which this script reads.
#
#   .\tools\dreamcast\run_flycast.ps1 -Cdi build\dc-debugoptimized\TRX.cdi -Seconds 30
param(
    [string]$Cdi = 'build\dc-debugoptimized\TRX.cdi',
    [int]$Seconds = 30,
    [string]$Flycast = 'G:\Flycast\flycast.exe',
    [string]$Out = 'build\dc-serial.txt',
    [switch]$KeepRunning,
    # The display to show the game on, by monitor name; empty leaves it
    # where Flycast puts it.
    [string]$Monitor = 'JMGO'
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
$proc = Start-Process $Flycast -ArgumentList "`"$cdiPath`"" `
    -WorkingDirectory (Split-Path $Flycast) -PassThru
Start-Sleep -Seconds 2
$readerProc = Start-Process powershell -WindowStyle Hidden -PassThru -ArgumentList `
    "-NoProfile -ExecutionPolicy Bypass -File `"$readerPath`" -ProcessId $($proc.Id) -OutPath `"$outPath`""
if ($Monitor -ne '') {
    try {
        & (Join-Path $PSScriptRoot 'flycast_place.ps1') -Monitor $Monitor
    } catch {
        "could not move Flycast: $_"
    }
}
# The serial console opens last and takes the focus; the game wants it.
try {
    & (Join-Path $PSScriptRoot 'flycast_keys.ps1') -FocusOnly
} catch {
    "could not focus Flycast: $_"
}

$deadline = $logStart.AddSeconds($Seconds)
while (-not $proc.HasExited -and (Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 500
}
if ($proc.HasExited) {
    "Flycast exited (code $($proc.ExitCode))"
} elseif (-not $KeepRunning) {
    Stop-Process -Id $proc.Id -Force
}
$readerProc.WaitForExit(5000) | Out-Null
"Serial output written to $Out"

$log = Join-Path (Split-Path $Flycast) 'flycast.log'
if ((Test-Path $log) -and (Get-Item $log).LastWriteTime -ge $logStart) {
    Get-Content $log | Select-String 'Fatal|exception|stopped' |
        Select-Object -Last 1 | ForEach-Object { "flycast: $($_.Line)" }
}
