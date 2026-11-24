param(
    [string]$RuntimeDir = (Join-Path $PSScriptRoot '..\dist\FPV GCS'),
    [string]$OutputDir = (Join-Path $PSScriptRoot '..\releases'),
    [string]$ISCC = (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe')
)
$ErrorActionPreference = 'Stop'
$fpvVersion = '0.1.10'
$fpvProject = Split-Path -Parent $PSScriptRoot
$fpvRepo = Split-Path -Parent $fpvProject
if (-not (Test-Path -LiteralPath $ISCC)) { throw 'Install Inno Setup 6 or pass -ISCC.' }
# A new staging directory prevents old executables, diagnostics and profiles leaking
# into releases. Never package the developer's dist directory recursively.
$fpvStage = Join-Path $env:TEMP ('FPVGCS-release-' + [guid]::NewGuid().ToString('N'))
$fpvPackage = Join-Path $fpvStage 'FPV GCS'
New-Item -ItemType Directory -Path $fpvPackage -Force | Out-Null
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
$fpvOutput = (Resolve-Path -LiteralPath $OutputDir).Path
foreach ($fpvFile in @('FPV GCS.exe', 'gst-plugin-scanner.exe')) {
    Copy-Item -LiteralPath (Join-Path $RuntimeDir $fpvFile) -Destination $fpvPackage
}
Copy-Item -Path (Join-Path $RuntimeDir '*.dll') -Destination $fpvPackage
foreach ($fpvFolder in @('gstreamer-1.0', 'platforms', 'iconengines', 'imageformats', 'styles', 'licenses')) {
    Copy-Item -LiteralPath (Join-Path $RuntimeDir $fpvFolder) -Destination $fpvPackage -Recurse
}
# Package committed documentation, including images referenced by the README.
# This also isolates unrelated working-tree edits and deleted local images.
$fpvDocsZip = Join-Path $fpvStage 'documentation.zip'
& git -C $fpvRepo archive --format=zip "--output=$fpvDocsZip" HEAD README.md RELEASE_NOTES.md docs
if ($LASTEXITCODE) { throw 'Cannot archive committed release documentation' }
Expand-Archive -LiteralPath $fpvDocsZip -DestinationPath $fpvPackage -Force
Copy-Item -LiteralPath (Join-Path $fpvProject 'README.md') -Destination (Join-Path $fpvPackage 'TECHNICAL.md')
Copy-Item -LiteralPath (Join-Path $fpvProject 'THIRD-PARTY-NOTICES.md') -Destination $fpvPackage
Copy-Item -LiteralPath (Join-Path $RuntimeDir 'test-results.txt') -Destination $fpvPackage
Copy-Item -LiteralPath (Join-Path $RuntimeDir 'capture-test-results.txt') -Destination $fpvPackage
$fpvCommit = & git -C $fpvRepo rev-parse HEAD
if ($LASTEXITCODE) { throw 'Cannot determine release commit' }
"FPV GCS $fpvVersion`nCommit: $fpvCommit" | Set-Content -LiteralPath (Join-Path $fpvPackage 'BUILD-INFO.txt') -Encoding ascii
$fpvZipName = "FPV-GCS-Portable-$fpvVersion-win64.zip"
Compress-Archive -LiteralPath $fpvPackage -DestinationPath (Join-Path $fpvOutput $fpvZipName) -CompressionLevel Optimal -Force
& $ISCC "/DPackageDir=$fpvPackage" "/DReleaseDir=$fpvOutput" (Join-Path $fpvProject 'packaging\installer.iss')
if ($LASTEXITCODE) { throw 'Installer compilation failed' }
$fpvSetupName = "FPV-GCS-Setup-$fpvVersion-win64.exe"
$fpvPiStage = Join-Path $fpvStage 'pi'
New-Item -ItemType Directory -Path (Join-Path $fpvPiStage 'systemd') -Force | Out-Null
Copy-Item -Path (Join-Path $fpvProject 'pi\*.py') -Destination $fpvPiStage
Copy-Item -LiteralPath (Join-Path $fpvProject 'pi\README.md') -Destination $fpvPiStage
foreach ($fpvPattern in @('*.py', '*.md', '*.service', '*.conf')) {
    Copy-Item -Path (Join-Path $fpvProject "pi\systemd\$fpvPattern") -Destination (Join-Path $fpvPiStage 'systemd')
}
Copy-Item -LiteralPath (Join-Path $fpvPackage 'BUILD-INFO.txt') -Destination $fpvPiStage
$fpvPiZipName = "FPV-GCS-Pi-$fpvVersion.zip"
Compress-Archive -LiteralPath $fpvPiStage -DestinationPath (Join-Path $fpvOutput $fpvPiZipName) -CompressionLevel Optimal -Force
$fpvChecksums = foreach ($fpvName in @($fpvSetupName, $fpvZipName, $fpvPiZipName)) {
    $fpvHash = (Get-FileHash -LiteralPath (Join-Path $fpvOutput $fpvName) -Algorithm SHA256).Hash.ToLowerInvariant()
    "$fpvHash  $fpvName"
}
$fpvChecksums | Set-Content -LiteralPath (Join-Path $fpvOutput "SHA256SUMS-$fpvVersion.txt") -Encoding ascii
Write-Output "Release files: $fpvOutput"
Write-Output "Staged runtime: $fpvPackage"
