# Re-shades an icon to aqua / turquoise / green: a gradient map on the picture's brightness (dark teal -> turquoise -> pale aqua), alpha untouched.
# Used by the build scripts when they take the launcher icon from the user's own APK, so the original artwork's colours are not shipped. (The same map is applied at run time to the window icon, see main.cpp.)
function Convert-IconTeal([System.Drawing.Bitmap]$bmp) {
    $stops = @( @(0.00, 2, 24, 32), @(0.45, 18, 160, 150), @(0.80, 110, 235, 210), @(1.00, 225, 255, 245) )
    for ($y = 0; $y -lt $bmp.Height; $y++) { for ($x = 0; $x -lt $bmp.Width; $x++) {
        $c = $bmp.GetPixel($x, $y); $l = (0.299 * $c.R + 0.587 * $c.G + 0.114 * $c.B) / 255.0
        for ($i = 1; $i -lt $stops.Count; $i++) { if ($l -le $stops[$i][0] -or $i -eq $stops.Count - 1) { $a = $stops[$i - 1]; $b = $stops[$i]; $t = [Math]::Min(1.0, [Math]::Max(0.0, ($l - $a[0]) / ($b[0] - $a[0])))
            $bmp.SetPixel($x, $y, [System.Drawing.Color]::FromArgb($c.A, [int]($a[1] + ($b[1] - $a[1]) * $t), [int]($a[2] + ($b[2] - $a[2]) * $t), [int]($a[3] + ($b[3] - $a[3]) * $t))); break } }
    } }
}