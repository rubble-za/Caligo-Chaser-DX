# Re-colours the glove in the launcher icon to aqua / turquoise / green and leaves everything else (the blue background) as it was.
# The glove is the red-orange part of the picture: pixels whose hue lies near red/orange (and that are clearly coloured) get their hue turned by +160 degrees, so red becomes teal-green and orange becomes aqua;
# the weight fades out over a band of hues so the edges stay smooth. Brightness, saturation and alpha are untouched. (The same rule is applied at run time to the window icon, see main.cpp.)
function Convert-IconTeal([System.Drawing.Bitmap]$bmp) {
    for ($y = 0; $y -lt $bmp.Height; $y++) { for ($x = 0; $x -lt $bmp.Width; $x++) {
        $c = $bmp.GetPixel($x, $y); $r = $c.R / 255.0; $g = $c.G / 255.0; $b = $c.B / 255.0
        $mx = [Math]::Max($r, [Math]::Max($g, $b)); $mn = [Math]::Min($r, [Math]::Min($g, $b)); $d = $mx - $mn
        if ($d -le 0.0001 -or $mx -le 0.0001) { continue }
        $s = $d / $mx
        if ($mx -eq $r) { $h = 60.0 * ((($g - $b) / $d) % 6) } elseif ($mx -eq $g) { $h = 60.0 * ((($b - $r) / $d) + 2) } else { $h = 60.0 * ((($r - $g) / $d) + 4) }
        if ($h -lt 0) { $h += 360.0 }
        $dh = [Math]::Abs($h - 15.0); if ($dh -gt 180) { $dh = 360 - $dh }                 # distance from red-orange
        $w = [Math]::Min(1.0, [Math]::Max(0.0, (60.0 - $dh) / 25.0)) * [Math]::Min(1.0, [Math]::Max(0.0, ($s - 0.2) / 0.2))
        if ($w -le 0) { continue }
        $h2 = ($h + 160.0 * $w) % 360.0; $cc = $mx * $s; $xx = $cc * (1 - [Math]::Abs((($h2 / 60.0) % 2) - 1)); $m = $mx - $cc
        switch ([int][Math]::Floor($h2 / 60.0)) { 0 { $o = @($cc, $xx, 0) } 1 { $o = @($xx, $cc, 0) } 2 { $o = @(0, $cc, $xx) } 3 { $o = @(0, $xx, $cc) } 4 { $o = @($xx, 0, $cc) } default { $o = @($cc, 0, $xx) } }
        $bmp.SetPixel($x, $y, [System.Drawing.Color]::FromArgb($c.A, [int](($o[0] + $m) * 255), [int](($o[1] + $m) * 255), [int](($o[2] + $m) * 255)))
    } }
}