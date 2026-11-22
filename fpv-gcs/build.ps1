param(
    [string]$GStreamerRoot = 'C:\gstreamer\1.0\msvc_x86_64',
    [string]$QtRoot = 'C:\Qt\6.11.1\mingw_64',
    [string]$CompilerRoot = 'C:\Qt\Tools\mingw1310_64\bin',
    [string]$CMake = 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe',
    [string]$Ninja = 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
)
$ErrorActionPreference = 'Stop'
$fpvProjectRoot = $PSScriptRoot
$env:PATH = "$CompilerRoot;$QtRoot\bin;$env:PATH"
$fpvOriginalSdlRoot = Join-Path $fpvProjectRoot '.deps\SDL3-3.4.20\x86_64-w64-mingw32'
if (-not (Test-Path -LiteralPath "$fpvOriginalSdlRoot\lib\cmake\SDL3\SDL3Config.cmake")) {
    $fpvDependencies = Join-Path $fpvProjectRoot '.deps'
    New-Item -ItemType Directory -Path $fpvDependencies -Force | Out-Null
    $fpvZip = Join-Path $fpvDependencies 'SDL3-devel-3.4.20-mingw.zip'
    Invoke-WebRequest 'https://github.com/libsdl-org/SDL/releases/download/release-3.4.20/SDL3-devel-3.4.20-mingw.zip' -OutFile $fpvZip
    if ((Get-FileHash -LiteralPath $fpvZip -Algorithm SHA256).Hash -ne '5D321D3D2D499CFABAD3023921E4B56BBEC03558060F4E195CE626D200126442') { throw 'SDL archive hash mismatch' }
    Expand-Archive -LiteralPath $fpvZip -DestinationPath $fpvDependencies -Force
}
# Qt's MinGW moc cannot reliably create output paths containing Cyrillic.
# Stage only this project's sources and SDK into a dedicated ASCII build directory.
$fpvBuildRoot = Join-Path $env:LOCALAPPDATA 'FPVGCSBuild'
$fpvStageSource = Join-Path $fpvBuildRoot 'source'
$fpvSdlRoot = Join-Path $fpvBuildRoot 'SDL3-3.4.20'
New-Item -ItemType Directory -Path $fpvStageSource -Force | Out-Null
foreach ($fpvSourceItem in @('src', 'tests', 'assets', 'CMakeLists.txt', 'README.md')) {
    Copy-Item -LiteralPath (Join-Path $fpvProjectRoot $fpvSourceItem) -Destination $fpvStageSource -Recurse -Force
}
if (-not (Test-Path -LiteralPath "$fpvSdlRoot\lib\cmake\SDL3\SDL3Config.cmake")) {
    Copy-Item -LiteralPath $fpvOriginalSdlRoot -Destination $fpvSdlRoot -Recurse -Force
}
& $CMake -S $fpvStageSource -B "$fpvBuildRoot\build" -G Ninja "-DCMAKE_MAKE_PROGRAM=$Ninja" "-DCMAKE_BUILD_TYPE=Release" "-DCMAKE_PREFIX_PATH=$QtRoot;$fpvSdlRoot" "-DCMAKE_CXX_COMPILER=$CompilerRoot\g++.exe" "-DGSTREAMER_ROOT=$GStreamerRoot"
if ($LASTEXITCODE) { throw 'CMake configure failed' }
& $CMake --build "$fpvBuildRoot\build" --target FPV_GCS fpv_tests --parallel 4
if ($LASTEXITCODE) { throw 'Build failed' }
& "$fpvBuildRoot\build\fpv_tests.exe" -o "$fpvBuildRoot\test-results.txt,txt"
if ($LASTEXITCODE) { throw 'Tests failed' }
Get-Content -LiteralPath "$fpvBuildRoot\test-results.txt"
$fpvDistribution = Join-Path $fpvBuildRoot 'package-rc'
New-Item -ItemType Directory -Path $fpvDistribution -Force | Out-Null
Copy-Item -LiteralPath "$fpvBuildRoot\build\FPV GCS.exe" -Destination $fpvDistribution
Copy-Item -LiteralPath "$fpvSdlRoot\bin\SDL3.dll" -Destination $fpvDistribution
# Keep the installed GStreamer C runtime and the required video plugins together.
# No gst-launch subprocess or system PATH configuration is needed at runtime.
Copy-Item -Path "$GStreamerRoot\bin\*.dll" -Destination $fpvDistribution -Force
$fpvGstPlugins = Join-Path $fpvDistribution 'gstreamer-1.0'
New-Item -ItemType Directory -Path $fpvGstPlugins -Force | Out-Null
foreach ($fpvPlugin in @('coreelements', 'udp', 'rtp', 'rtpmanager', 'videoparsersbad', 'playback', 'typefindfunctions', 'd3d11', 'libav')) {
    Copy-Item -LiteralPath "$GStreamerRoot\lib\gstreamer-1.0\gst$fpvPlugin.dll" -Destination $fpvGstPlugins -Force
}
Copy-Item -LiteralPath "$GStreamerRoot\libexec\gstreamer-1.0\gst-plugin-scanner.exe" -Destination $fpvDistribution -Force
& "$QtRoot\bin\windeployqt.exe" --release --no-translations --no-opengl-sw --no-system-d3d-compiler --compiler-runtime --skip-plugin-types generic,networkinformation,tls --dir $fpvDistribution "$fpvDistribution\FPV GCS.exe"
if ($LASTEXITCODE) { throw 'Qt deployment failed' }
Copy-Item -LiteralPath "$fpvProjectRoot\README.md" -Destination $fpvDistribution
New-Item -ItemType Directory -Path "$fpvDistribution\pi" -Force | Out-Null
Copy-Item -Path "$fpvProjectRoot\pi\*.py" -Destination "$fpvDistribution\pi" -Force
$fpvLicenseRoot = Join-Path $fpvDistribution 'licenses'
New-Item -ItemType Directory -Path $fpvLicenseRoot -Force | Out-Null
Copy-Item -Path "$GStreamerRoot\share\licenses" -Destination (Join-Path $fpvLicenseRoot 'gstreamer') -Recurse -Force
Copy-Item -LiteralPath (Join-Path $fpvProjectRoot '.deps\SDL3-3.4.20\LICENSE.txt') -Destination (Join-Path $fpvLicenseRoot 'SDL3.txt')
foreach ($fpvQtModule in @('qtbase', 'qtsvg')) {
    $fpvQtLicenseSource = Join-Path (Split-Path -Parent $QtRoot) "Src\$fpvQtModule\LICENSES"
    if (Test-Path -LiteralPath $fpvQtLicenseSource) {
        $fpvQtLicenseDestination = Join-Path $fpvLicenseRoot $fpvQtModule
        New-Item -ItemType Directory -Path $fpvQtLicenseDestination -Force | Out-Null
        Copy-Item -Path "$fpvQtLicenseSource\*" -Destination $fpvQtLicenseDestination -Force
    }
}
$fpvFinalParent = Join-Path $fpvProjectRoot 'dist'
New-Item -ItemType Directory -Path $fpvFinalParent -Force | Out-Null
$fpvFinal = Join-Path $fpvFinalParent 'FPV GCS'
New-Item -ItemType Directory -Path $fpvFinal -Force | Out-Null
# Remove only superseded generated networking plugins from the initial package.
foreach ($fpvGeneratedName in @('generic', 'networkinformation', 'tls')) {
    $fpvGeneratedPath = Join-Path $fpvFinal $fpvGeneratedName
    if (Test-Path -LiteralPath $fpvGeneratedPath) {
        $fpvResolvedFinal = (Resolve-Path -LiteralPath $fpvFinal).Path
        $fpvResolvedGenerated = (Resolve-Path -LiteralPath $fpvGeneratedPath).Path
        if ((Split-Path -Parent $fpvResolvedGenerated) -ne $fpvResolvedFinal) { throw 'Unexpected package cleanup path' }
        Remove-Item -LiteralPath $fpvResolvedGenerated -Recurse -Force
    }
}
Copy-Item -Path "$fpvDistribution\*" -Destination $fpvFinal -Recurse -Force
Copy-Item -LiteralPath "$fpvBuildRoot\test-results.txt" -Destination (Join-Path $fpvFinal 'test-results.txt')
Write-Output "Built: $fpvFinal\FPV GCS.exe"
