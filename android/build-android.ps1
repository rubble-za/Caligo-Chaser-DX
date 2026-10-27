# Builds the Android debug APK (arm64). Needs: JDK 17, Android SDK (platform 34, build-tools 34, cmake 3.22.1, NDK 26.3.11579264) in C:\android-sdk, Git for Windows, Gradle 8.7 in C:\caligo-chaser-build\gradle-8.7.
# Unicorn's configure step breaks on paths with spaces, so dependencies and the native build live under C:\caligo-chaser-build (the same folder build-windows.ps1 uses).
# Output: C:\caligo-chaser-build\android-gradle\outputs\apk\debug\app-debug.apk
param([string]$IconFromApk = '', [switch]$OriginalIcon); $ErrorActionPreference = 'Continue'; $root = Split-Path $PSScriptRoot; $B = 'C:\caligo-chaser-build'; $sdk = 'C:\android-sdk'; $ndk = "$sdk\ndk\26.3.11579264"; $T = "$B\third_party"
$env:JAVA_HOME = (Get-ChildItem 'C:\Program Files\Microsoft\jdk-17*' | Select-Object -First 1).FullName; $env:ANDROID_HOME = $sdk; $env:CC_BUILD_ROOT = $(if ($env:CC_BUILD_ROOT_OVERRIDE) { $env:CC_BUILD_ROOT_OVERRIDE } else { $B })
$env:Path = "$T\stubs;C:\Program Files\Git\usr\bin;C:\Program Files\Git\cmd;$env:Path"
if (-not (Test-Path "$T\dynarmic")) { Write-Host 'Run build-windows.ps1 once first (it fetches the dependencies into C:\caligo-chaser-build\third_party).'; exit 1 }
# Optional: use the original game's launcher icon (from your own APK) instead of the neutral built-in one. The icon is Com2uS's artwork: it stays on this machine and is not committed.
Remove-Item Env:CC_ICON_DIR -ErrorAction SilentlyContinue
if ($IconFromApk -and (Test-Path $IconFromApk)) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem; Add-Type -AssemblyName System.Drawing; . "$root\tools\icon-tint.ps1"
    $z = [IO.Compression.ZipFile]::OpenRead($IconFromApk); $icons = "$B\icons"; Remove-Item $icons -Recurse -Force -ErrorAction SilentlyContinue
    try {
        $src = @{ 'ldpi' = 'res/drawable-ldpi/icon.png'; 'mdpi' = 'res/drawable-mdpi/icon.png'; 'hdpi' = 'res/drawable-hdpi/icon.png' }
        foreach ($d in $src.Keys) { $e = $z.GetEntry($src[$d]); if ($e) { New-Item -ItemType Directory -Force "$icons\mipmap-$d" | Out-Null; [IO.Compression.ZipFileExtensions]::ExtractToFile($e, "$icons\mipmap-$d\ic_launcher.png", $true)
            if (-not $OriginalIcon) { $img = [Drawing.Bitmap]::FromFile("$icons\mipmap-$d\ic_launcher.png"); $bm = New-Object Drawing.Bitmap $img; $img.Dispose(); Convert-IconTeal $bm; $bm.Save("$icons\mipmap-$d\ic_launcher.png", [Drawing.Imaging.ImageFormat]::Png); $bm.Dispose() } } }
        if (Test-Path "$icons\mipmap-hdpi\ic_launcher.png") {
            $hd = [Drawing.Image]::FromFile("$icons\mipmap-hdpi\ic_launcher.png")
            foreach ($d in @{ 'xhdpi' = 96; 'xxhdpi' = 144; 'xxxhdpi' = 192 }.GetEnumerator()) {          # the APK only has up to hdpi (72 px): scale it up for sharper screens
                New-Item -ItemType Directory -Force "$icons\mipmap-$($d.Key)" | Out-Null; $bmp = New-Object Drawing.Bitmap $d.Value, $d.Value; $g = [Drawing.Graphics]::FromImage($bmp)
                $g.InterpolationMode = 'HighQualityBicubic'; $g.PixelOffsetMode = 'HighQuality'; $g.DrawImage($hd, 0, 0, $d.Value, $d.Value); $g.Dispose(); $bmp.Save("$icons\mipmap-$($d.Key)\ic_launcher.png", [Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
            }
            $hd.Dispose(); $env:CC_ICON_DIR = $icons; Write-Host "Using the launcher icon from $IconFromApk"
        } else { Write-Host 'No launcher icon found in that APK; using the neutral icon.' }
    } finally { $z.Dispose() }
}& "$sdk\cmake\3.22.1\bin\cmake.exe" -S "$root\android\app\jni" -B "$B\android" -G Ninja "-DCMAKE_MAKE_PROGRAM=$sdk\cmake\3.22.1\bin\ninja.exe" "-DCMAKE_TOOLCHAIN_FILE=$ndk\build\cmake\android.toolchain.cmake" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=Release "-DT=$($T -replace '\\','/')"
& "$sdk\cmake\3.22.1\bin\ninja.exe" -C "$B\android" SDL2 main
$j = "$root\android\app\src\main\jniLibs\arm64-v8a"; New-Item -ItemType Directory -Force $j | Out-Null
Copy-Item "$B\android\libmain.so", "$B\android\SDL2\libSDL2.so", "$ndk\toolchains\llvm\prebuilt\windows-x86_64\sysroot\usr\lib\aarch64-linux-android\libc++_shared.so" $j
Set-Location "$root\android"; "sdk.dir=$($sdk -replace '\\','/')" | Set-Content local.properties -Encoding ascii
& "$B\gradle-8.7\bin\gradle.bat" assembleDebug --no-daemon