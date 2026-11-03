# Builds and tests inside a queue job, so it never overlaps another agent's test (AGENTS.md rule 6):
#   queue.ps1 submit -Name '...' -File tools\build_queued.ps1 -TimeoutMinutes 10
$ErrorActionPreference = 'Stop'
$env:ACB_EXE = "D:\Program Files\Games\Ubisoft\Assassin's Creed Brotherhood\ACBSP.exe"
& cmd.exe /d /c "`"$PSScriptRoot\..\build-msvc.cmd`""
exit $LASTEXITCODE
