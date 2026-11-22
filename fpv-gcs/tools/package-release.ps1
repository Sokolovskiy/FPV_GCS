param(
    [string]$RuntimeDir = (Join-Path $PSScriptRoot '..\dist\FPV GCS'),
    [string]$OutputDir = (Join-Path $PSScriptRoot '..\releases'),
    [string]$ISCC = (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe')
)
$ErrorActionPreference = 'Stop'
$fpvVersion = '0.1.9'
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
Copy-Item -LiteralPath (Join-Path $fpvRepo 'README.md') -Destination $fpvPackage
Copy-Item -LiteralPath (Join-Path $fpvProject 'THIRD-PARTY-NOTICES.md') -Destination $fpvPackage
Copy-Item -LiteralPath (Join-Path $RuntimeDir 'test-results.txt') -Destination $fpvPackage
$fpvZipName = "FPV-GCS-Portable-$fpvVersion-win64.zip"
Compress-Archive -LiteralPath $fpvPackage -DestinationPath (Join-Path $fpvOutput $fpvZipName) -CompressionLevel Optimal -Force
& $ISCC "/DPackageDir=$fpvPackage" "/DReleaseDir=$fpvOutput" (Join-Path $fpvProject 'packaging\installer.iss')
if ($LASTEXITCODE) { throw 'Installer compilation failed' }
$fpvSetupName = "FPV-GCS-Setup-$fpvVersion-win64.exe"
$fpvChecksums = foreach ($fpvName in @($fpvSetupName, $fpvZipName)) {
    $fpvHash = (Get-FileHash -LiteralPath (Join-Path $fpvOutput $fpvName) -Algorithm SHA256).Hash.ToLowerInvariant()
    "$fpvHash  $fpvName"
}
$fpvChecksums | Set-Content -LiteralPath (Join-Path $fpvOutput 'SHA256SUMS.txt') -Encoding ascii
Write-Output "Release files: $fpvOutput"
Write-Output "Staged runtime: $fpvPackage"
