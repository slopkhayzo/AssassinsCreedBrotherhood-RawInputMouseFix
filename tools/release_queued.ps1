# Builds, tests, packages and verifies a release inside one queue job (AGENTS.md rule 6):
#   queue.ps1 submit -Name '...' -File tools\release_queued.ps1 -Arguments 1.0.0-rc1 -TimeoutMinutes 10
param([string]$Version = '1.0.0-rc1')

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

& "$PSScriptRoot\build_queued.ps1"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& "$repo\package-release.ps1" -Version $Version -LoaderPath "$repo\research\thirdparty\ual-x86-v9.7.4\dinput8.dll"

# Verify: every archive's payload matches its SHA256SUMS.txt, and the archive hash file matches.
Add-Type -AssemblyName System.IO.Compression.FileSystem
$failures = 0
foreach ($zip in Get-ChildItem "$repo\dist" -Filter "ACBrotherhood-RawInputMouseFix-v$Version-*.zip") {
    $check = Join-Path $repo ("build\release-verify\" + $zip.BaseName)
    if (Test-Path -LiteralPath $check) { Remove-Item -LiteralPath $check -Recurse -Force }
    [System.IO.Compression.ZipFile]::ExtractToDirectory($zip.FullName, $check)
    Write-Host "== $($zip.Name)"
    foreach ($line in Get-Content -LiteralPath (Join-Path $check 'SHA256SUMS.txt')) {
        $hash, $name = $line -split '  ', 2
        $actual = (Get-FileHash -LiteralPath (Join-Path $check $name) -Algorithm SHA256).Hash.ToLowerInvariant()
        $ok = $actual -eq $hash
        if (-not $ok) { $failures++ }
        Write-Host ("  {0} {1} ({2} bytes)" -f ($(if ($ok) { 'ok  ' } else { 'FAIL' })), $name, (Get-Item (Join-Path $check $name)).Length)
    }
    $expected = ((Get-Content -LiteralPath ($zip.FullName + '.sha256')) -split '  ')[0]
    $actualZip = (Get-FileHash -LiteralPath $zip.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($expected -ne $actualZip) { $failures++; Write-Host '  FAIL archive hash' } else { Write-Host "  ok   archive $actualZip" }
}
if ($failures -gt 0) { Write-Host "$failures verification failure(s)"; exit 1 }
exit 0
