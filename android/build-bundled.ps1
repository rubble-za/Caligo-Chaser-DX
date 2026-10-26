# Builds a PRIVATE test APK that carries your own game files (the original APK and the Com2uS data folder) and unpacks them on first launch, so each phone needs only this one file.
# The result contains Com2uS's copyrighted files: use it on your own devices only and never share or commit it.
# Usage:  build-bundled.ps1 -GameApk "C:\path\CaligoChaser_1.0.4.apk" -DataDir "C:\path\folder that contains Com2uS" [-Out "C:\path\CaligoChaser-bundled.apk"]
param([Parameter(Mandatory)][string]$GameApk, [Parameter(Mandatory)][string]$DataDir, [string]$Out = (Join-Path $env:USERPROFILE 'CaligoChaser-android-bundled.apk'))
$ErrorActionPreference = 'Stop'
$B = 'C:\caligo-chaser-build'; $bundle = "$B\bundle-assets"
if (-not (Test-Path "$DataDir\Com2uS\com.com2us.caligo\resources.csr")) { throw "Com2uS\com.com2us.caligo\resources.csr not found under $DataDir" }
Remove-Item $bundle -Recurse -Force -ErrorAction SilentlyContinue; New-Item -ItemType Directory -Force $bundle | Out-Null
Copy-Item $GameApk "$bundle\CaligoChaser.apk"; Copy-Item "$DataDir\Com2uS" "$bundle\Com2uS" -Recurse
$files = @('CaligoChaser.apk') + (Get-ChildItem "$bundle\Com2uS" -Recurse -File | ForEach-Object { $_.FullName.Substring($bundle.Length + 1).Replace('\', '/') })
[IO.File]::WriteAllText("$bundle\files.txt", ($files -join "`n"), (New-Object Text.UTF8Encoding($false)))
$env:CC_BUNDLE_DIR = $bundle; $env:CC_BUILD_ROOT_OVERRIDE = "$B\bundled"
& "$PSScriptRoot\build-android.ps1"
$apk = "$B\bundled\android-gradle\outputs\apk\debug\app-debug.apk"
if (-not (Test-Path $apk)) { $apk = "$B\android-gradle\outputs\apk\debug\app-debug.apk" }
Copy-Item $apk $Out -Force; Write-Host "Built $Out ($([math]::Round((Get-Item $Out).Length / 1MB, 1)) MB) - private, do not share"