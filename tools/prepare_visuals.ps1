# Prepare the HOME banner and top-screen boot splash only. Never edits icon.png.
$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
Add-Type -AssemblyName System.Drawing

function New-ScaledImage {
    param([System.Drawing.Image]$Source, [int]$Width, [int]$Height, [bool]$CenterCrop)
    $sourceRect = [System.Drawing.Rectangle]::new(0, 0, $Source.Width, $Source.Height)
    if ($CenterCrop) {
        $sourceRatio = [double]$Source.Width / $Source.Height
        $targetRatio = [double]$Width / $Height
        if ($sourceRatio -gt $targetRatio) {
            $cropWidth = [int][Math]::Round($Source.Height * $targetRatio)
            $sourceRect = [System.Drawing.Rectangle]::new(
                [int](($Source.Width - $cropWidth) / 2), 0, $cropWidth, $Source.Height)
        } elseif ($sourceRatio -lt $targetRatio) {
            $cropHeight = [int][Math]::Round($Source.Width / $targetRatio)
            $sourceRect = [System.Drawing.Rectangle]::new(
                0, [int](($Source.Height - $cropHeight) / 2), $Source.Width, $cropHeight)
        }
    }
    $result = [System.Drawing.Bitmap]::new($Width, $Height,
        [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $graphics = [System.Drawing.Graphics]::FromImage($result)
    try {
        $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $graphics.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
        $graphics.DrawImage($Source, [System.Drawing.Rectangle]::new(0, 0, $Width, $Height),
            $sourceRect, [System.Drawing.GraphicsUnit]::Pixel)
    } finally { $graphics.Dispose() }
    return $result
}

$bannerSource = [System.Drawing.Image]::FromFile((Join-Path $project 'assets\banner-source.png'))
try {
    if ($bannerSource.Width -ne 2 * $bannerSource.Height) {
        throw 'La banniere source doit etre au format 2:1.'
    }
    $banner = New-ScaledImage -Source $bannerSource -Width 256 -Height 128 -CenterCrop $false
    try { $banner.Save((Join-Path $project 'banner.png'), [System.Drawing.Imaging.ImageFormat]::Png) }
    finally { $banner.Dispose() }
} finally { $bannerSource.Dispose() }

$splashSource = [System.Drawing.Image]::FromFile((Join-Path $project 'assets\boot-top-source.png'))
try {
    $splash = New-ScaledImage -Source $splashSource -Width 400 -Height 240 -CenterCrop $true
    try {
        $splash.Save((Join-Path $project 'assets\boot-top-preview.png'),
            [System.Drawing.Imaging.ImageFormat]::Png)
        $raw = [byte[]]::new(400 * 240 * 3)
        for ($x = 0; $x -lt 400; $x++) {
            for ($y = 0; $y -lt 240; $y++) {
                $pixel = $splash.GetPixel($x, $y)
                $offset = ($x * 240 + (239 - $y)) * 3
                $raw[$offset] = $pixel.B
                $raw[$offset + 1] = $pixel.G
                $raw[$offset + 2] = $pixel.R
            }
        }
        [System.IO.File]::WriteAllBytes((Join-Path $project 'romfs\boot-top.bgr'), $raw)
    } finally { $splash.Dispose() }
} finally { $splashSource.Dispose() }
