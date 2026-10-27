# Pixel-art helpers for LiftOff textures (PowerShell 7 + System.Drawing, no Python).
#
# Dot-source it:  . "$PSScriptRoot/lib.ps1"
#
# A texture is a list of equal-length strings ("rows"); each character is a palette key.
# '.' (or any key mapped to $null / 'transparent') is a transparent pixel.
# PowerShell hashtable literals are case-insensitive: don't use keys that differ only by case.
#
#   $rows = @(
#       '..oo..',
#       '.oMMo.'
#   )
#   $pal  = @{ o = '#3b2412'; M = '#8b5a2b' }
#   Save-PixelArt -Rows $rows -Palette $pal -Path 'assets/pack/rp/textures/items/foo.png'
#
# Helpers:
#   ConvertTo-Color '#rrggbb' | '#aarrggbb' | [Drawing.Color]
#   Shade-Color $color 0.8     -> darker (factor < 1) / lighter (factor > 1)
#   Mix-Color $a $b 0.5        -> linear blend
#   Save-PixelArt -Rows -Palette -Path [-Scale n]   (Scale > 1 writes an enlarged preview)
#   Merge-Rows $base $overlay  -> overlay's non-'.' chars replace base chars (same size)

Add-Type -AssemblyName System.Drawing

function ConvertTo-Color($value) {
    if ($null -eq $value) { return [System.Drawing.Color]::Transparent }
    if ($value -is [System.Drawing.Color]) { return $value }
    $s = "$value".Trim()
    if ($s -eq 'transparent') { return [System.Drawing.Color]::Transparent }
    if ($s.StartsWith('#')) { $s = $s.Substring(1) }
    if ($s.Length -eq 6) { $s = 'FF' + $s }
    if ($s.Length -ne 8) { throw "Bad color '$value' (use #rrggbb or #aarrggbb)" }
    return [System.Drawing.Color]::FromArgb([Convert]::ToInt32($s, 16))
}

function Clamp255([double]$v) { return [int][Math]::Max(0, [Math]::Min(255, [Math]::Round($v))) }

function Shade-Color($color, [double]$factor) {
    $c = ConvertTo-Color $color
    if ($factor -le 1) {
        return [System.Drawing.Color]::FromArgb($c.A, (Clamp255 ($c.R * $factor)), (Clamp255 ($c.G * $factor)), (Clamp255 ($c.B * $factor)))
    }
    $t = $factor - 1
    return [System.Drawing.Color]::FromArgb($c.A, (Clamp255 ($c.R + (255 - $c.R) * $t)), (Clamp255 ($c.G + (255 - $c.G) * $t)), (Clamp255 ($c.B + (255 - $c.B) * $t)))
}

function Mix-Color($a, $b, [double]$t) {
    $x = ConvertTo-Color $a; $y = ConvertTo-Color $b
    return [System.Drawing.Color]::FromArgb(
        (Clamp255 ($x.A + ($y.A - $x.A) * $t)), (Clamp255 ($x.R + ($y.R - $x.R) * $t)),
        (Clamp255 ($x.G + ($y.G - $x.G) * $t)), (Clamp255 ($x.B + ($y.B - $x.B) * $t)))
}

function Merge-Rows([string[]]$Base, [string[]]$Overlay) {
    if ($Base.Count -ne $Overlay.Count) { throw "Merge-Rows: row count differs" }
    $out = for ($y = 0; $y -lt $Base.Count; $y++) {
        $b = $Base[$y].ToCharArray(); $o = $Overlay[$y]
        for ($x = 0; $x -lt $b.Length; $x++) { if ($x -lt $o.Length -and $o[$x] -ne '.') { $b[$x] = $o[$x] } }
        -join $b
    }
    return , $out
}

function Save-PixelArt {
    param(
        [Parameter(Mandatory)][string[]]$Rows,
        [Parameter(Mandatory)][hashtable]$Palette,
        [Parameter(Mandatory)][string]$Path,
        [int]$Scale = 1
    )
    $h = $Rows.Count
    $w = ($Rows | Measure-Object -Property Length -Maximum).Maximum
    foreach ($r in $Rows) { if ($r.Length -ne $w) { throw "Save-PixelArt: all rows must have length $w ('$r')" } }
    $colors = [System.Collections.Generic.Dictionary[string, System.Drawing.Color]]::new([StringComparer]::Ordinal)
    foreach ($k in $Palette.Keys) { $colors[[string]$k] = ConvertTo-Color $Palette[$k] }
    $bmp = New-Object System.Drawing.Bitmap ($w * $Scale), ($h * $Scale), ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    try {
        for ($y = 0; $y -lt $h; $y++) {
            for ($x = 0; $x -lt $w; $x++) {
                $ch = [string]$Rows[$y][$x]
                if ($ch -eq '.') { $c = [System.Drawing.Color]::Transparent }
                elseif ($colors.ContainsKey($ch)) { $c = $colors[$ch] }
                else { throw "Save-PixelArt: no palette entry for '$ch' (row $y)" }
                for ($sy = 0; $sy -lt $Scale; $sy++) { for ($sx = 0; $sx -lt $Scale; $sx++) { $bmp.SetPixel($x * $Scale + $sx, $y * $Scale + $sy, $c) } }
            }
        }
        $dir = Split-Path -Parent $Path
        if ($dir) { New-Item -ItemType Directory -Force $dir | Out-Null }
        $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    } finally { $bmp.Dispose() }
}
