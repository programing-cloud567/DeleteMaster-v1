Add-Type -AssemblyName System.Drawing

$outDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $outDir
$icoPath = Join-Path $root 'src\host\app.ico'
$previewPath = Join-Path $outDir 'icon-preview.png'

function New-IconBitmap([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)

    $cx = [single]($size / 2)
    $cy = [single]($size / 2)
    $R = [single]($size * 0.455)
    $red = [System.Drawing.Color]::FromArgb(255, 206, 32, 39)
    $black = [System.Drawing.Color]::FromArgb(255, 22, 22, 24)
    $white = [System.Drawing.Color]::White

    $wb = New-Object System.Drawing.SolidBrush($white)
    $g.FillEllipse($wb, [single]($cx - $R), [single]($cy - $R), [single]($R * 2), [single]($R * 2))

    $bagW = [single]($size * 0.30)
    $bagH = [single]($size * 0.34)
    $bagX = [single]($cx - $bagW / 2)
    $bagY = [single]($size * 0.395)

    $bb = New-Object System.Drawing.SolidBrush($black)
    $g.FillEllipse($bb, $bagX, $bagY, $bagW, $bagH)
    $g.FillRectangle($bb, [single]($cx - $size * 0.045), [single]($size * 0.345), [single]($size * 0.09), [single]($size * 0.09))

    $earL = New-Object System.Drawing.Drawing2D.GraphicsPath
    $earL.AddPolygon(@(
        (New-Object System.Drawing.PointF([single]($cx - $size * 0.045), [single]($size * 0.352))),
        (New-Object System.Drawing.PointF([single]($cx - $size * 0.125), [single]($size * 0.296))),
        (New-Object System.Drawing.PointF([single]($cx - $size * 0.035), [single]($size * 0.310)))
    ))
    $g.FillPath($bb, $earL)
    $earR = New-Object System.Drawing.Drawing2D.GraphicsPath
    $earR.AddPolygon(@(
        (New-Object System.Drawing.PointF([single]($cx + $size * 0.045), [single]($size * 0.352))),
        (New-Object System.Drawing.PointF([single]($cx + $size * 0.125), [single]($size * 0.296))),
        (New-Object System.Drawing.PointF([single]($cx + $size * 0.035), [single]($size * 0.310)))
    ))
    $g.FillPath($bb, $earR)
    $g.FillEllipse($bb, [single]($cx - $size * 0.032), [single]($size * 0.306), [single]($size * 0.064), [single]($size * 0.052))

    $barW = [single]($size * 0.088)
    $bar = New-Object System.Drawing.Pen($red, $barW)
    $bar.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $bar.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $k = [single]($R * 0.70)
    $g.DrawLine($bar, [single]($cx - $k), [single]($cy - $k), [single]($cx + $k), [single]($cy + $k))

    $ring = New-Object System.Drawing.Pen($red, [single]($size * 0.075))
    $inset = [single]($size * 0.0375)
    $g.DrawEllipse($ring, [single]($cx - $R + $inset), [single]($cy - $R + $inset),
                   [single]($R * 2 - $inset * 2), [single]($R * 2 - $inset * 2))

    $ring.Dispose(); $bar.Dispose(); $earR.Dispose(); $earL.Dispose()
    $bb.Dispose(); $wb.Dispose(); $g.Dispose()
    return $bmp
}

function New-IconEntry([int]$size) {
    $bmp = New-IconBitmap $size
    $hicon = $bmp.GetHicon()
    $icon = [System.Drawing.Icon]::FromHandle($hicon)
    $ms = New-Object System.IO.MemoryStream
    $icon.Save($ms)
    $raw = $ms.ToArray()
    $ms.Dispose(); $icon.Dispose(); $bmp.Dispose()

    $offset = [BitConverter]::ToInt32($raw, 6 + 12)
    $length = [BitConverter]::ToInt32($raw, 6 + 8)
    $payload = New-Object 'System.Byte[]' $length
    [Array]::Copy($raw, $offset, $payload, 0, $length)
    $bits = [BitConverter]::ToUInt16($raw, 6 + 6)
    return [PSCustomObject]@{ Size = $size; Data = $payload; Bits = $bits }
}

$preview = New-IconBitmap 256
$onWhite = New-Object System.Drawing.Bitmap(256, 256)
$og = [System.Drawing.Graphics]::FromImage($onWhite)
$og.Clear([System.Drawing.Color]::FromArgb(255, 245, 245, 245))
$og.DrawImage($preview, 0, 0)
$og.Dispose()
$onWhite.Save($previewPath, [System.Drawing.Imaging.ImageFormat]::Png)
$onWhite.Dispose()
$preview.Dispose()

$sizes = @(16, 24, 32, 48, 64, 128, 256)
$entries = @()
foreach ($s in $sizes) { $entries += (New-IconEntry $s) }

$fs = [System.IO.File]::Create($icoPath)
$bw = New-Object System.IO.BinaryWriter($fs)
$bw.Write([UInt16]0); $bw.Write([UInt16]1); $bw.Write([UInt16]$entries.Count)
$dataOffset = 6 + 16 * $entries.Count
foreach ($e in $entries) {
    $dim = $e.Size
    if ($dim -ge 256) { $dim = 0 }
    $bw.Write([Byte]$dim); $bw.Write([Byte]$dim); $bw.Write([Byte]0); $bw.Write([Byte]0)
    $bw.Write([UInt16]1); $bw.Write([UInt16]$e.Bits)
    $bw.Write([UInt32]$e.Data.Length); $bw.Write([UInt32]$dataOffset)
    $dataOffset += $e.Data.Length
}
foreach ($e in $entries) { $bw.Write($e.Data) }
$bw.Flush(); $bw.Close(); $fs.Close()

Write-Output ("icon: " + $icoPath + " bytes=" + (Get-Item $icoPath).Length)
Write-Output ("preview: " + $previewPath)
