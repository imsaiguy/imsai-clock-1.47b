# Exports the digit shapes of an installed Windows font as 8-bit coverage masks (for tools\gen_jelly.py).
# File format (little endian): count(u16), then per glyph: x(i16) y(i16) w(u16) h(u16) followed by w*h bytes.
# x and y are the tight box's position on a common canvas, so the digits keep their relative baseline.
param([string]$Family = "Arial Rounded MT Bold", [string]$Out = "$PSScriptRoot\..\assets\masks.bin", [int]$Px = 420)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
$fs = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter $fs
$bw.Write([uint16]10)
foreach ($ch in '0','1','2','3','4','5','6','7','8','9') {
    $f = New-Object System.Drawing.Font $Family, ([single]$Px), ([System.Drawing.FontStyle]::Regular), ([System.Drawing.GraphicsUnit]::Pixel)
    $sf = New-Object System.Drawing.StringFormat ([System.Drawing.StringFormat]::GenericTypographic)
    $cs = 900
    $b = New-Object System.Drawing.Bitmap $cs, $cs, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($b); $g.Clear([System.Drawing.Color]::Black)
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAlias
    $g.DrawString($ch, $f, [System.Drawing.Brushes]::White, 100, 60, $sf); $g.Dispose(); $f.Dispose()
    $bd = $b.LockBits((New-Object System.Drawing.Rectangle 0,0,$cs,$cs), [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $raw = New-Object byte[] ($bd.Stride * $cs)
    [System.Runtime.InteropServices.Marshal]::Copy($bd.Scan0, $raw, 0, $raw.Length); $stride = $bd.Stride; $b.UnlockBits($bd); $b.Dispose()
    $minx = $cs; $maxx = -1; $miny = $cs; $maxy = -1
    for ($y = 0; $y -lt $cs; $y++) { for ($x = 0; $x -lt $cs; $x++) {
        if ($raw[$y * $stride + $x * 4 + 2] -gt 6) { if ($x -lt $minx) {$minx=$x}; if ($x -gt $maxx) {$maxx=$x}; if ($y -lt $miny) {$miny=$y}; if ($y -gt $maxy) {$maxy=$y} } } }
    $w = $maxx - $minx + 1; $h = $maxy - $miny + 1
    $bw.Write([int16]$minx); $bw.Write([int16]$miny); $bw.Write([uint16]$w); $bw.Write([uint16]$h)
    $a = New-Object byte[] ($w * $h)
    for ($y = 0; $y -lt $h; $y++) { for ($x = 0; $x -lt $w; $x++) { $a[$y * $w + $x] = $raw[($miny + $y) * $stride + ($minx + $x) * 4 + 2] } }
    $bw.Write($a)
}
$bw.Flush()
$null = New-Item -ItemType Directory -Force (Split-Path $Out)
[System.IO.File]::WriteAllBytes($Out, $fs.ToArray())
"Wrote $Out ($Family, $Px px)"
