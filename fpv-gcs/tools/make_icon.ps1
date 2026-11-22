param(
    [string]$Source = (Join-Path $PSScriptRoot '../assets/fatum-icon-source.png'),
    [string]$Destination = (Join-Path $PSScriptRoot '../assets/fatum.ico')
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$iconSource = [System.Drawing.Image]::FromFile((Resolve-Path -LiteralPath $Source).Path)
try {
    $iconSizes = @(16, 24, 32, 48, 64, 128, 256)
    $iconFrames = @()
    foreach ($iconSize in $iconSizes) {
        $iconBitmap = [System.Drawing.Bitmap]::new($iconSize, $iconSize)
        $iconGraphics = [System.Drawing.Graphics]::FromImage($iconBitmap)
        $iconStream = [System.IO.MemoryStream]::new()
        try {
            $iconGraphics.Clear([System.Drawing.Color]::Transparent)
            $iconGraphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $iconScale = [Math]::Min($iconSize / $iconSource.Width, $iconSize / $iconSource.Height)
            $iconWidth = [Math]::Max(1, [int][Math]::Round($iconSource.Width * $iconScale))
            $iconHeight = [Math]::Max(1, [int][Math]::Round($iconSource.Height * $iconScale))
            $iconRectangle = [System.Drawing.Rectangle]::new([int](($iconSize-$iconWidth)/2), [int](($iconSize-$iconHeight)/2), $iconWidth, $iconHeight)
            $iconGraphics.DrawImage($iconSource, $iconRectangle)
            $iconBitmap.Save($iconStream, [System.Drawing.Imaging.ImageFormat]::Png)
            $iconFrames += ,$iconStream.ToArray()
        } finally { $iconGraphics.Dispose(); $iconBitmap.Dispose(); $iconStream.Dispose() }
    }
    $iconWriter = [System.IO.BinaryWriter]::new([System.IO.File]::Create([System.IO.Path]::GetFullPath($Destination)))
    try {
        $iconWriter.Write([uint16]0); $iconWriter.Write([uint16]1); $iconWriter.Write([uint16]$iconSizes.Count)
        $iconOffset = 6 + 16 * $iconSizes.Count
        for ($iconIndex = 0; $iconIndex -lt $iconSizes.Count; $iconIndex++) {
            $iconDimension = if ($iconSizes[$iconIndex] -eq 256) { 0 } else { $iconSizes[$iconIndex] }
            $iconWriter.Write([byte]$iconDimension); $iconWriter.Write([byte]$iconDimension)
            $iconWriter.Write([byte]0); $iconWriter.Write([byte]0)
            $iconWriter.Write([uint16]1); $iconWriter.Write([uint16]32)
            $iconWriter.Write([uint32]$iconFrames[$iconIndex].Length); $iconWriter.Write([uint32]$iconOffset)
            $iconOffset += $iconFrames[$iconIndex].Length
        }
        foreach ($iconFrame in $iconFrames) { $iconWriter.Write([byte[]]$iconFrame) }
    } finally { $iconWriter.Dispose() }
} finally { $iconSource.Dispose() }
