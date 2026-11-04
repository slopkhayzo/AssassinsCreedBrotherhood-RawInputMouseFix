# Builds and tests inside a queue job, so it never overlaps another agent's test (AGENTS.md rule 6):
#   queue.ps1 submit -Name '...' -File tools\build_queued.ps1 -TimeoutMinutes 10
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$env:ACB_EXE = "D:\Program Files\Games\Ubisoft\Assassin's Creed Brotherhood\ACBSP.exe"
# The Steam build and an analysis copy unpacked with Steamless (research\bin\steam, never shipped).
$steamExe = 'D:\Program Files\Games\Steam\steamapps\common\Assassins Creed Brotherhood\ACBSP.exe'
$steamUnpacked = Join-Path $repo 'research\bin\steam\ACBSP.exe.unpacked.exe'
if ((Test-Path -LiteralPath $steamExe) -and (Test-Path -LiteralPath $steamUnpacked)) {
    $env:ACB_STEAM_EXE = $steamExe
    $env:ACB_STEAM_UNPACKED = $steamUnpacked
}
& cmd.exe /d /c "`"$PSScriptRoot\..\build-msvc.cmd`""
exit $LASTEXITCODE
