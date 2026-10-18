# Runs a command inside the DreamSDK shell (KallistiOS environment loaded),
# from the current directory.
# Example: .\tools\dreamcast\dcsh.ps1 "bash tools/dreamcast/build.sh"
param([Parameter(Mandatory = $true)][string]$Command)
if (-not $env:DREAMSDK_HOME) { $env:DREAMSDK_HOME = 'G:\DreamSDK' }
$env:MSYSTEM = 'MINGW64'
$bash = Join-Path $env:DREAMSDK_HOME 'usr\bin\bash.exe'
$cwd = (Get-Location).Path -replace '\\', '/'
$tmp = [System.IO.Path]::GetTempFileName() + '.sh'
@"
source /opt/toolchains/dc/kos/environ.sh
export PATH="`$PATH:/c/Program Files/Git/cmd"
cd "$cwd" || exit 1
$Command
"@ | Set-Content -Path $tmp -Encoding ascii
$tmpUnix = $tmp -replace '\\', '/'
& $bash -l $tmpUnix 2>&1 | Select-Object -Skip 9 | ForEach-Object { "$_" }
$code = $LASTEXITCODE
Remove-Item $tmp -ErrorAction SilentlyContinue
exit $code
