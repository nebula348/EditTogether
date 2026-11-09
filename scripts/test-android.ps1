# Builds the test suites for arm64-v8a and runs them on every connected adb device.
#   powershell -ExecutionPolicy Bypass -File scripts\test-android.ps1
param(
    [string]$Ndk = "$env:LOCALAPPDATA\Android\Sdk\ndk\29.0.14206865",
    [string]$Adb = "adb"
)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root "build-tests-android"

cmake -S "$root\tests" -B $build -G Ninja `
    "-DCMAKE_TOOLCHAIN_FILE=$Ndk\build\cmake\android.toolchain.cmake" `
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-23 -DANDROID_STL=c++_static `
    -DCMAKE_BUILD_TYPE=RelWithDebInfo | Out-Null
cmake --build $build
if ($LASTEXITCODE -ne 0) { throw "test build failed" }

$devices = & $Adb devices | Select-Object -Skip 1 | Where-Object { $_ -match "\tdevice$" } | ForEach-Object { ($_ -split "\t")[0] }
if (-not $devices) { Write-Host "Built in $build. No adb device connected, nothing run."; exit 0 }

$failed = 0
foreach ($d in $devices) {
    Write-Host "=== $d ==="
    foreach ($t in "core_tests", "net_tests", "relay_tests") {
        & $Adb -s $d push "$build\$t" "/data/local/tmp/$t" | Out-Null
        & $Adb -s $d shell "chmod 755 /data/local/tmp/$t && /data/local/tmp/$t"
        if ($LASTEXITCODE -ne 0) { $failed++ }
    }
}
exit $failed
