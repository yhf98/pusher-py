[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Platform,
    [string]$BuildDir = $(Join-Path $PSScriptRoot "..\build\sdk"),
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"

$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$VersionHeader = Join-Path $Root "include\pusher\version.h"
$VersionLine = Get-Content -LiteralPath $VersionHeader | Where-Object { $_ -match '^\s*#define\s+PUSHER_VERSION\s+"([^"]+)"' } | Select-Object -First 1
if (!$VersionLine) {
    throw "Unable to read PUSHER_VERSION from $VersionHeader"
}
$Version = [regex]::Match($VersionLine, '"([^"]+)"').Groups[1].Value

$PackageName = "pusher-sdk-$Version-$Platform"
$DistDir = Join-Path $Root "dist-sdk"
$StageDir = Join-Path $DistDir $PackageName
$Archive = Join-Path $DistDir "$PackageName.zip"
$ConfigDir = Join-Path $BuildDir $Config

Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $StageDir, $Archive
New-Item -ItemType Directory -Force -Path `
    (Join-Path $StageDir "bin"), `
    (Join-Path $StageDir "examples"), `
    (Join-Path $StageDir "include"), `
    (Join-Path $StageDir "lib") | Out-Null

Copy-Item -Recurse -Force -LiteralPath (Join-Path $Root "include\pusher") -Destination (Join-Path $StageDir "include")
Copy-Item -Force -LiteralPath `
    (Join-Path $Root "examples\c_demo.c"), `
    (Join-Path $Root "examples\cpp_demo.cpp") `
    -Destination (Join-Path $StageDir "examples")
Copy-Item -Force -LiteralPath `
    (Join-Path $Root "README.md"), `
    (Join-Path $Root "SDK_API.md"), `
    (Join-Path $Root "CMakeLists.txt") `
    -Destination $StageDir

foreach ($Path in @(
    (Join-Path $ConfigDir "pusher.dll"),
    (Join-Path $ConfigDir "pusher_c_demo.exe"),
    (Join-Path $ConfigDir "pusher_cpp_demo.exe")
)) {
    if (Test-Path $Path) {
        Copy-Item -Force -LiteralPath $Path -Destination (Join-Path $StageDir "bin")
    }
}

foreach ($Path in @(
    (Join-Path $ConfigDir "pusher.lib")
)) {
    if (Test-Path $Path) {
        Copy-Item -Force -LiteralPath $Path -Destination (Join-Path $StageDir "lib")
    }
}

Get-ChildItem -LiteralPath (Join-Path $Root "lib") -File -Force -ErrorAction SilentlyContinue |
    Where-Object { $_.Name.EndsWith(".dll", [StringComparison]::OrdinalIgnoreCase) } |
    ForEach-Object { Copy-Item -Force -LiteralPath $_.FullName -Destination (Join-Path $StageDir "bin") }

Get-ChildItem -LiteralPath (Join-Path $Root "lib") -File -Force -ErrorAction SilentlyContinue |
    Where-Object { $_.Name.EndsWith(".lib", [StringComparison]::OrdinalIgnoreCase) } |
    ForEach-Object { Copy-Item -Force -LiteralPath $_.FullName -Destination (Join-Path $StageDir "lib") }

@"
pusher native SDK $Version ($Platform)

Contents:
- include\pusher: public C ABI and C++ headers
- bin: pusher.dll, FFmpeg runtime DLLs, MSVC runtime DLLs, and demo programs
- lib: pusher.lib and FFmpeg import libraries
- examples: demo source code

Runtime:
  set PATH=%CD%\bin;%PATH%

Dry-run demos:
  bin\pusher_c_demo.exe sample.mp4 rtmp://127.0.0.1:1935/live/test
  bin\pusher_cpp_demo.exe "video=Integrated Camera" rtmp://127.0.0.1:1935/live/camera

Start a real push only when the target server is ready:
  bin\pusher_cpp_demo.exe "video=Integrated Camera" rtmp://SERVER/live/camera0 --start
"@ | Set-Content -LiteralPath (Join-Path $StageDir "README_SDK.txt") -Encoding UTF8

Push-Location $DistDir
try {
    Compress-Archive -Force -Path $PackageName -DestinationPath $Archive
} finally {
    Pop-Location
}

Write-Host "Created $Archive"
