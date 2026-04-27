[CmdletBinding()]
param(
    [ValidateSet("x86", "AMD64", "ARM64")]
    [string]$Arch = $(if ($env:CIBW_ARCHS_WINDOWS) { $env:CIBW_ARCHS_WINDOWS } else { "AMD64" })
)

$ErrorActionPreference = "Stop"

$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$SourceDir = Join-Path $Root "third_party\FFmpeg"
$BuildDir = Join-Path $Root "build\ffmpeg-windows\$Arch"
$Prefix = Join-Path $Root ".ffmpeg-prefix\windows-$Arch"
$IncludeDir = Join-Path $Root "include"
$LibDir = Join-Path $Root "lib"

if (!(Test-Path $SourceDir)) {
    throw "FFmpeg source not found: $SourceDir"
}

$RequiredSources = @(
    "configure",
    "Makefile",
    "ffbuild\common.mak",
    "ffbuild\library.mak",
    "libavformat\Makefile",
    "libavcodec\Makefile",
    "libavutil\Makefile"
)
$MissingSources = @(
    $RequiredSources | Where-Object { !(Test-Path (Join-Path $SourceDir $_)) }
)
if ($MissingSources.Count -gt 0) {
    $MissingText = $MissingSources -join ", "
    throw "FFmpeg source tree is incomplete: $SourceDir. Missing required files: $MissingText. Commit the full third_party/FFmpeg source tree to GitHub, including Makefile and ffbuild/*.mak."
}

$ToolPaths = @{}
foreach ($tool in @("cl.exe", "link.exe", "lib.exe")) {
    $command = Get-Command $tool -ErrorAction SilentlyContinue
    if (!$command) {
        throw "$tool was not found. Run this script from a Visual Studio Developer shell, or use the GitHub Actions workflow."
    }
    $ToolPaths[$tool] = $command.Source
}
Write-Host "Using MSVC cl.exe: $($ToolPaths['cl.exe'])"
Write-Host "Using MSVC link.exe: $($ToolPaths['link.exe'])"
Write-Host "Using MSVC lib.exe: $($ToolPaths['lib.exe'])"

$BashCandidates = @(
    @(
        "$env:MSYS2_ROOT\usr\bin\bash.exe",
        "C:\msys64\usr\bin\bash.exe",
        "C:\msys2\usr\bin\bash.exe"
    ) | Where-Object { $_ -and (Test-Path $_) }
)

if ($BashCandidates.Count -eq 0) {
    $bashCommand = Get-Command "bash.exe" -ErrorAction SilentlyContinue
    if ($bashCommand) {
        $BashCandidates = @([string]$bashCommand.Source)
    }
}

if ($BashCandidates.Count -eq 0) {
    throw "MSYS2 bash.exe was not found. Install MSYS2 or run the GitHub Actions workflow."
}

$Bash = $BashCandidates[0]
Write-Host "Using MSYS2 bash: $Bash"

switch ($Arch.ToUpperInvariant()) {
    "X86" {
        $FfmpegArch = "x86"
        $TargetOs = "win32"
    }
    "AMD64" {
        $FfmpegArch = "x86_64"
        $TargetOs = "win64"
    }
    "ARM64" {
        $FfmpegArch = "aarch64"
        $TargetOs = "win64"
    }
}

function Convert-ToMsysPath([string]$Path) {
    $escaped = $Path.Replace("'", "'\''")
    return (& $Bash -lc "cygpath -u '$escaped'").Trim()
}

function Quote-Sh([string]$Value) {
    return "'" + $Value.Replace("'", "'\''") + "'"
}

$RootUnix = Convert-ToMsysPath $Root
$SourceUnix = Convert-ToMsysPath $SourceDir
$BuildUnix = Convert-ToMsysPath $BuildDir
$PrefixUnix = Convert-ToMsysPath $Prefix
$MsvcBinUnix = Convert-ToMsysPath (Split-Path -Parent $ToolPaths["cl.exe"])

$RequiredUnixSources = @(
    "$SourceUnix/configure",
    "$SourceUnix/Makefile",
    "$SourceUnix/ffbuild/common.mak",
    "$SourceUnix/ffbuild/library.mak",
    "$SourceUnix/libavformat/Makefile",
    "$SourceUnix/libavcodec/Makefile",
    "$SourceUnix/libavutil/Makefile"
)
foreach ($SourceFile in $RequiredUnixSources) {
    $CheckCommand = "test -f $(Quote-Sh $SourceFile)"
    & $Bash -lc $CheckCommand
    if ($LASTEXITCODE -ne 0) {
        throw "Missing FFmpeg source file visible from MSYS2: $SourceFile. Commit the full third_party/FFmpeg source tree to GitHub before building wheels."
    }
}

$ConfigureArgs = @(
    "--prefix=$PrefixUnix",
    "--toolchain=msvc",
    "--target-os=$TargetOs",
    "--arch=$FfmpegArch",
    "--enable-shared",
    "--disable-static",
    "--disable-programs",
    "--disable-doc",
    "--disable-debug",
    "--disable-autodetect",
    "--disable-asm",
    "--disable-x86asm",
    "--disable-avfilter",
    "--disable-avdevice",
    "--disable-swscale",
    "--disable-swresample",
    "--enable-network",
    "--disable-everything",
    "--enable-avformat",
    "--enable-avcodec",
    "--enable-avutil",
    "--enable-protocol=file,pipe,tcp,udp,rtmp,rtmpt,rtsp,http,rtp",
    "--enable-demuxer=mov,mp4,m4a,3gp,3g2,mj2,flv,rtsp,rtp,mpegts,h264,hevc,aac,matroska",
    "--enable-muxer=flv,rtsp,rtp,mpegts,mp4,null",
    "--enable-parser=h264,hevc,aac,mpeg4video",
    "--enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,aac_adtstoasc"
)

if ($Arch.ToUpperInvariant() -eq "ARM64") {
    $ConfigureArgs += "--enable-cross-compile"
}

$ConfigureCommand = ((@("$SourceUnix/configure") + $ConfigureArgs) | ForEach-Object { Quote-Sh $_ }) -join " "

New-Item -ItemType Directory -Force -Path $BuildDir, $Prefix, $IncludeDir, $LibDir | Out-Null
$env:MSYS2_PATH_TYPE = "inherit"

$BuildUnixQ = Quote-Sh $BuildUnix
$PrefixUnixQ = Quote-Sh $PrefixUnix
$MsvcBinUnixQ = Quote-Sh $MsvcBinUnix
$RootIncludeQ = Quote-Sh "$RootUnix/include"
$RootLibQ = Quote-Sh "$RootUnix/lib"
$PrefixIncludeQ = Quote-Sh "$PrefixUnix/include/"
$PrefixLibQ = Quote-Sh "$PrefixUnix/lib/"
$PrefixBinQ = Quote-Sh "$PrefixUnix/bin/"
$BuildAvformatQ = Quote-Sh "$BuildUnix/libavformat/"
$BuildAvcodecQ = Quote-Sh "$BuildUnix/libavcodec/"
$BuildAvutilQ = Quote-Sh "$BuildUnix/libavutil/"

$ScriptLines = @(
    "set -euo pipefail",
    "export PATH=${MsvcBinUnixQ}:`$PATH:/usr/bin",
    "mkdir -p $BuildUnixQ $PrefixUnixQ $RootIncludeQ $RootLibQ",
    "cd $BuildUnixQ",
    "echo `"MSYS2 cl.exe: `$(command -v cl.exe || true)`"",
    "echo `"MSYS2 link.exe: `$(command -v link.exe || true)`"",
    "echo `"MSYS2 lib.exe: `$(command -v lib.exe || true)`"",
    "if [ ! -f config.mak ]; then $ConfigureCommand; fi",
    "echo `"FFmpeg selected build variables:`"",
    "/usr/bin/grep -E '^(CONFIG_SHARED|CONFIG_STATIC|CONFIG_AVFORMAT|CONFIG_AVCODEC|CONFIG_AVUTIL|CC=|LD=|AR=|SLIBNAME|SLIBNAME_WITH_MAJOR|SLIBSUF|LIBSUF|SHFLAGS=)' ffbuild/config.mak || true",
    "echo `"Using make: `$(command -v make)`"",
    "/usr/bin/make -j`$(/usr/bin/nproc) libavutil/avutil.dll libavcodec/avcodec.dll libavformat/avformat.dll",
    "/usr/bin/make install-libs install-headers",
    "echo `"Windows FFmpeg build-tree artifacts:`"",
    "/usr/bin/find . -maxdepth 3 -type f \( -name '*.dll' -o -name '*.lib' -o -name '*.def' -o -name '*.dll.a' \) -print | /usr/bin/sort",
    "echo `"Windows FFmpeg prefix artifacts:`"",
    "/usr/bin/find $PrefixUnixQ -maxdepth 4 -type f \( -name '*.dll' -o -name '*.lib' -o -name '*.def' -o -name '*.dll.a' \) -print | /usr/bin/sort",
    "/usr/bin/cp -a ${PrefixIncludeQ}* $RootIncludeQ",
    "for pattern in ${PrefixLibQ}*.lib ${PrefixBinQ}*.lib ${PrefixBinQ}*.dll ${BuildAvformatQ}*.lib ${BuildAvformatQ}*.dll ${BuildAvcodecQ}*.lib ${BuildAvcodecQ}*.dll ${BuildAvutilQ}*.lib ${BuildAvutilQ}*.dll; do if [ -e `"`$pattern`" ]; then /usr/bin/cp -a `"`$pattern`" $RootLibQ; fi; done",
    "/usr/bin/find $PrefixUnixQ $BuildUnixQ -maxdepth 4 -type f \( -name '*.dll' -o -name '*.lib' \) -exec /usr/bin/cp -a {} $RootLibQ \;",
    "echo `"Windows FFmpeg SDK lib directory:`"",
    "/usr/bin/find $RootLibQ -maxdepth 1 -type f | /usr/bin/sort"
)
$Script = $ScriptLines -join "`n"

& $Bash -lc $Script
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$RequiredComponents = @("avformat", "avcodec", "avutil")

$ArtifactRoots = @($BuildDir, $Prefix) | Where-Object { Test-Path $_ }
Write-Host "PowerShell FFmpeg artifact scan:"
$AllArtifacts = @()
foreach ($ArtifactRoot in $ArtifactRoots) {
    $Artifacts = Get-ChildItem -Path $ArtifactRoot -Recurse -Force -ErrorAction SilentlyContinue |
        Where-Object {
            !$_.PSIsContainer -and (
                $_.Name.EndsWith(".dll", [StringComparison]::OrdinalIgnoreCase) -or
                $_.Name.EndsWith(".lib", [StringComparison]::OrdinalIgnoreCase) -or
                $_.Name.EndsWith(".def", [StringComparison]::OrdinalIgnoreCase) -or
                $_.Name.EndsWith(".dll.a", [StringComparison]::OrdinalIgnoreCase)
            )
        } |
        Sort-Object FullName
    foreach ($Artifact in $Artifacts) {
        Write-Host "  $($Artifact.FullName)"
        $AllArtifacts += $Artifact
    }
}

foreach ($Artifact in $AllArtifacts) {
    if (
        $Artifact.Name.EndsWith(".dll", [StringComparison]::OrdinalIgnoreCase) -or
        $Artifact.Name.EndsWith(".lib", [StringComparison]::OrdinalIgnoreCase)
    ) {
        Copy-Item -Force -LiteralPath $Artifact.FullName -Destination $LibDir
    }
}

foreach ($Component in $RequiredComponents) {
    $PlainImportLib = Join-Path $LibDir "$Component.lib"
    $PrefixedImportLib = Join-Path $LibDir "lib$Component.lib"
    if (!(Test-Path $PlainImportLib) -and (Test-Path $PrefixedImportLib)) {
        Copy-Item -Force $PrefixedImportLib $PlainImportLib
    }

    if (!(Test-Path $PlainImportLib)) {
        $DefinitionFile = $AllArtifacts |
            Where-Object { $_.Name -like "$Component*.def" -or $_.Name -like "lib$Component*.def" } |
            Select-Object -First 1
        if ($DefinitionFile) {
            $Machine = switch ($Arch.ToUpperInvariant()) {
                "X86" { "X86" }
                "AMD64" { "X64" }
                "ARM64" { "ARM64" }
            }
            Write-Host "Creating $Component.lib from $($DefinitionFile.FullName)"
            & $ToolPaths["lib.exe"] /NOLOGO "/MACHINE:$Machine" "/DEF:$($DefinitionFile.FullName)" "/OUT:$PlainImportLib"
            if ($LASTEXITCODE -ne 0) {
                exit $LASTEXITCODE
            }
        }
    }
}

$MissingArtifacts = @()
foreach ($Component in $RequiredComponents) {
    $PlainImportLib = Join-Path $LibDir "$Component.lib"
    $PrefixedImportLib = Join-Path $LibDir "lib$Component.lib"
    if (!(Test-Path $PlainImportLib) -and !(Test-Path $PrefixedImportLib)) {
        $MissingArtifacts += "$Component import library (.lib)"
    }

    $RuntimeDlls = Get-ChildItem -Path $LibDir -Filter "*$Component*.dll" -Force -ErrorAction SilentlyContinue
    if (!$RuntimeDlls) {
        $MissingArtifacts += "$Component runtime DLL (.dll)"
    }
}

if ($MissingArtifacts.Count -gt 0) {
    Write-Host "Files found in ${LibDir}:"
    Get-ChildItem -Path $LibDir -Force -ErrorAction SilentlyContinue | Sort-Object Name | ForEach-Object { Write-Host "  $($_.Name)" }
    foreach ($ArtifactRoot in $ArtifactRoots) {
        Write-Host "Files found under ${ArtifactRoot}:"
        Get-ChildItem -Path $ArtifactRoot -Recurse -Force -ErrorAction SilentlyContinue |
            Sort-Object FullName |
            Select-Object -First 300 |
            ForEach-Object { Write-Host "  $($_.FullName)" }
    }
    $ConfigMak = Join-Path $BuildDir "ffbuild\config.mak"
    if (Test-Path $ConfigMak) {
        Write-Host "ffbuild/config.mak relevant lines:"
        Get-Content $ConfigMak |
            Select-String -Pattern "^(CONFIG_SHARED|CONFIG_STATIC|CONFIG_AVFORMAT|CONFIG_AVCODEC|CONFIG_AVUTIL|CC=|LD=|AR=|SLIBNAME|SLIBNAME_WITH_MAJOR|SLIBSUF|LIBSUF|SHFLAGS=)" |
            ForEach-Object { Write-Host "  $($_.Line)" }
    }
    $MissingText = $MissingArtifacts -join ", "
    throw "FFmpeg Windows SDK build is incomplete. Missing: $MissingText"
}

Write-Host "FFmpeg SDK built into:"
Write-Host "  $IncludeDir"
Write-Host "  $LibDir"
