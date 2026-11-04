param(
    [ValidatePattern('^\d+\.\d+\.\d+(?:-[0-9A-Za-z.]+)?$')]
    [string]$Version = '1.0.1',
    [string]$LoaderPath
)

$ErrorActionPreference = 'Stop'

$repositoryRoot = [System.IO.Path]::GetFullPath($PSScriptRoot)
$buildDirectory = Join-Path $repositoryRoot 'build'
$distDirectory = Join-Path $repositoryRoot 'dist'
$stagingRoot = Join-Path $buildDirectory 'release-staging'
$plugin = Join-Path $buildDirectory 'ACBRawMouseFix.asi'
$configuration = Join-Path $repositoryRoot 'acb-raw-mouse-fix.ini'
$readmeTemplate = Join-Path $repositoryRoot 'release-readme.txt'
$projectLicense = Join-Path $repositoryRoot 'LICENSE'
$thirdPartyNotices = Join-Path $repositoryRoot 'THIRD-PARTY-NOTICES.txt'
$loaderVersion = '9.7.4'
$expectedLoaderHash = 'D5A059AA467A7A7127C8F6169F79FA63FF0F55986EE9EB2FD9A281BEBF2AA2E6'
# THIRD-PARTY-NOTICES.txt: everything before this line applies to every archive (MinHook is
# compiled into the ASI); the rest only to the archive that bundles the loader.
$loaderNoticeMarker = '--- Ultimate ASI Loader'

foreach ($requiredFile in @($plugin, $configuration, $readmeTemplate, $projectLicense, $thirdPartyNotices)) {
    if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
        throw "Required release input is missing: $requiredFile"
    }
}

function Confirm-ChildPath {
    param([string]$Path, [string]$Parent)

    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $fullParent = [System.IO.Path]::GetFullPath($Parent).TrimEnd('\') + '\'
    if (-not $fullPath.StartsWith($fullParent, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to modify a path outside $fullParent : $fullPath"
    }
    return $fullPath
}

function Reset-StagingDirectory {
    param([string]$Path)

    $safePath = Confirm-ChildPath -Path $Path -Parent $buildDirectory
    if (Test-Path -LiteralPath $safePath) {
        Remove-Item -LiteralPath $safePath -Recurse -Force
    }
    New-Item -ItemType Directory -Path $safePath | Out-Null
    return $safePath
}

function Write-PayloadHashes {
    param([string]$Directory)

    $lines = Get-ChildItem -LiteralPath $Directory -File |
        Where-Object Name -ne 'SHA256SUMS.txt' |
        Sort-Object Name |
        ForEach-Object {
            $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
            '{0}  {1}' -f $hash.ToLowerInvariant(), $_.Name
        }
    Set-Content -LiteralPath (Join-Path $Directory 'SHA256SUMS.txt') -Value $lines -Encoding Ascii
}

function Write-Notices {
    param([string]$Destination, [bool]$IncludeLoader)

    $text = Get-Content -LiteralPath $thirdPartyNotices -Raw
    $marker = $text.IndexOf($loaderNoticeMarker)
    if ($marker -lt 0) { throw "THIRD-PARTY-NOTICES.txt has no '$loaderNoticeMarker' section" }
    if (-not $IncludeLoader) { $text = $text.Substring(0, $marker).TrimEnd() + "`r`n" }
    Set-Content -LiteralPath $Destination -Value $text -Encoding UTF8 -NoNewline
}

function New-ReleaseArchive {
    param(
        [string]$Name,
        [string]$Stage,
        [bool]$IncludeLoader,
        [string]$ResolvedLoader
    )

    $stagePath = Reset-StagingDirectory -Path $Stage
    Copy-Item -LiteralPath $plugin -Destination $stagePath
    Copy-Item -LiteralPath $configuration -Destination $stagePath
    Copy-Item -LiteralPath $projectLicense -Destination (Join-Path $stagePath 'LICENSE.txt')
    Write-Notices -Destination (Join-Path $stagePath 'THIRD-PARTY-NOTICES.txt') -IncludeLoader $IncludeLoader

    $releaseReadme = (Get-Content -LiteralPath $readmeTemplate -Raw).Replace('@VERSION@', $Version)
    Set-Content -LiteralPath (Join-Path $stagePath 'README.txt') -Value $releaseReadme -Encoding UTF8

    if ($IncludeLoader) {
        Copy-Item -LiteralPath $ResolvedLoader -Destination (Join-Path $stagePath 'dinput8.dll')
    }

    Write-PayloadHashes -Directory $stagePath

    $archive = Join-Path $distDirectory ($Name + '.zip')
    $archiveHash = $archive + '.sha256'
    if ((Test-Path -LiteralPath $archive) -or (Test-Path -LiteralPath $archiveHash)) {
        throw "Release output already exists; refusing to overwrite: $archive"
    }

    Compress-Archive -Path (Join-Path $stagePath '*') -DestinationPath $archive
    $zipHash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
    $zipHashLine = '{0}  {1}' -f $zipHash.ToLowerInvariant(), [System.IO.Path]::GetFileName($archive)
    Set-Content -LiteralPath $archiveHash -Value $zipHashLine -Encoding Ascii
    Write-Host "created $archive"
    Write-Host "  SHA-256 $zipHash"
}

New-Item -ItemType Directory -Force -Path $distDirectory | Out-Null

$pluginOnlyName = "ACBrotherhood-RawInputMouseFix-v$Version-plugin-only"
New-ReleaseArchive -Name $pluginOnlyName -Stage (Join-Path $stagingRoot 'plugin-only') `
    -IncludeLoader $false -ResolvedLoader ''

if ($LoaderPath) {
    $resolvedLoader = (Resolve-Path -LiteralPath $LoaderPath).Path
    $loaderItem = Get-Item -LiteralPath $resolvedLoader
    if ($loaderItem.Name -ine 'dinput8.dll') {
        throw "Ultimate ASI Loader must be supplied as dinput8.dll: $resolvedLoader"
    }
    if ($loaderItem.VersionInfo.FileVersion -ne $loaderVersion) {
        throw "Loader version is $($loaderItem.VersionInfo.FileVersion); expected $loaderVersion."
    }
    $actualLoaderHash = (Get-FileHash -LiteralPath $resolvedLoader -Algorithm SHA256).Hash
    if ($actualLoaderHash -ne $expectedLoaderHash) {
        throw "Loader hash mismatch: $actualLoaderHash"
    }

    $bundleName = "ACBrotherhood-RawInputMouseFix-v$Version-with-Ultimate-ASI-Loader-v$loaderVersion"
    New-ReleaseArchive -Name $bundleName -Stage (Join-Path $stagingRoot 'with-loader') `
        -IncludeLoader $true -ResolvedLoader $resolvedLoader
}
