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
    "libavutil\Makefile",
    "libavdevice\Makefile",
    "libswscale\Makefile",
    "libswresample\Makefile"
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

if ($Arch.ToUpperInvariant() -eq "X86" -and $ToolPaths["cl.exe"] -match "\\bin\\HostX86\\x86\\cl\.exe$") {
    $HostX64X86Bin = $ToolPaths["cl.exe"] -replace "\\bin\\HostX86\\x86\\cl\.exe$", "\bin\HostX64\x86"
    $HostX64X86Cl = Join-Path $HostX64X86Bin "cl.exe"
    $HostX64X86Link = Join-Path $HostX64X86Bin "link.exe"
    $HostX64X86Lib = Join-Path $HostX64X86Bin "lib.exe"
    if ((Test-Path $HostX64X86Cl) -and (Test-Path $HostX64X86Link) -and (Test-Path $HostX64X86Lib)) {
        Write-Host "Switching Windows x86 FFmpeg build to 64-bit-hosted MSVC tools to avoid HostX86 compiler ICE."
        $ToolPaths["cl.exe"] = $HostX64X86Cl
        $ToolPaths["link.exe"] = $HostX64X86Link
        $ToolPaths["lib.exe"] = $HostX64X86Lib
    }
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

$MsysUsrBin = Split-Path -Parent $Bash
$MsysMake = Join-Path $MsysUsrBin "make.exe"
$MsysPacman = Join-Path $MsysUsrBin "pacman.exe"
if (!(Test-Path $MsysMake) -and (Test-Path $MsysPacman)) {
    Write-Host "MSYS make.exe not found at $MsysMake. Installing MSYS package: make"
    & $Bash -lc "pacman -S --needed --noconfirm make"
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}
if (!(Test-Path $MsysMake)) {
    throw "MSYS make.exe was not found at $MsysMake. Install the MSYS2 'make' package, not the MinGW make package."
}
Write-Host "Using MSYS make.exe: $MsysMake"

switch ($Arch.ToUpperInvariant()) {
    "X86" {
        $FfmpegArch = "x86"
        $TargetOs = "win32"
        $VCRedistArch = "x86"
    }
    "AMD64" {
        $FfmpegArch = "x86_64"
        $TargetOs = "win64"
        $VCRedistArch = "x64"
    }
    "ARM64" {
        $FfmpegArch = "aarch64"
        $TargetOs = "win64"
        $VCRedistArch = "arm64"
    }
}

function Find-VCRuntimeDir([string]$RedistArch) {
    $CandidateRoots = @()

    if ($env:VCToolsRedistDir) {
        $CandidateRoots += $env:VCToolsRedistDir
    }

    $ClPath = $ToolPaths["cl.exe"]
    $ClMatch = [regex]::Match($ClPath, "^(.*\\Microsoft Visual Studio\\[^\\]+\\[^\\]+\\VC)\\Tools\\MSVC\\")
    if ($ClMatch.Success) {
        $CandidateRoots += (Join-Path $ClMatch.Groups[1].Value "Redist\MSVC")
    }

    foreach ($VsEdition in @("Enterprise", "Professional", "Community", "BuildTools")) {
        if ($env:ProgramFiles) {
            $CandidateRoots += (Join-Path $env:ProgramFiles "Microsoft Visual Studio\2022\$VsEdition\VC\Redist\MSVC")
        }
        if (${env:ProgramFiles(x86)}) {
            $CandidateRoots += (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\2022\$VsEdition\VC\Redist\MSVC")
        }
    }

    foreach ($Root in ($CandidateRoots | Where-Object { $_ } | Select-Object -Unique)) {
        if (!(Test-Path $Root)) {
            continue
        }

        $Direct = Join-Path $Root "$RedistArch\Microsoft.VC143.CRT"
        if (Test-Path (Join-Path $Direct "msvcp140.dll")) {
            return $Direct
        }

        $VersionedDirs = Get-ChildItem -LiteralPath $Root -Directory -Force -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending
        foreach ($VersionedDir in $VersionedDirs) {
            $Candidate = Join-Path $VersionedDir.FullName "$RedistArch\Microsoft.VC143.CRT"
            if (Test-Path (Join-Path $Candidate "msvcp140.dll")) {
                return $Candidate
            }
        }
    }

    return $null
}

function Copy-VCRuntimeDlls([string]$RedistArch, [string]$DestinationDir) {
    $RuntimeDir = Find-VCRuntimeDir $RedistArch
    if (!$RuntimeDir) {
        throw "MSVC runtime redist directory was not found for $RedistArch. Expected Microsoft.VC143.CRT with msvcp140.dll."
    }

    Write-Host "Using MSVC runtime redist: $RuntimeDir"
    $RuntimeDlls = Get-ChildItem -LiteralPath $RuntimeDir -File -Force -ErrorAction SilentlyContinue |
        Where-Object {
            $_.Name -like "msvcp140*.dll" -or
            $_.Name -like "vcruntime140*.dll" -or
            $_.Name -ieq "concrt140.dll"
        } |
        Sort-Object Name

    if (!$RuntimeDlls) {
        throw "No MSVC runtime DLLs found in $RuntimeDir"
    }

    foreach ($RuntimeDll in $RuntimeDlls) {
        Write-Host "Copying MSVC runtime DLL: $($RuntimeDll.Name)"
        Copy-Item -Force -LiteralPath $RuntimeDll.FullName -Destination $DestinationDir
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
$MsysMakeUnix = Convert-ToMsysPath $MsysMake

$RequiredUnixSources = @(
    "$SourceUnix/configure",
    "$SourceUnix/Makefile",
    "$SourceUnix/ffbuild/common.mak",
    "$SourceUnix/ffbuild/library.mak",
    "$SourceUnix/libavformat/Makefile",
    "$SourceUnix/libavcodec/Makefile",
    "$SourceUnix/libavutil/Makefile",
    "$SourceUnix/libavdevice/Makefile",
    "$SourceUnix/libswscale/Makefile",
    "$SourceUnix/libswresample/Makefile"
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
    "--enable-avdevice",
    "--enable-swscale",
    "--enable-swresample",
    "--enable-network",
    "--disable-everything",
    "--enable-avformat",
    "--enable-avcodec",
    "--enable-avutil",
    "--enable-swscale",
    "--enable-swresample",
    "--enable-avdevice",
    "--enable-protocol=file,pipe,tcp,udp,rtmp,rtmpt,rtsp,http,rtp",
    "--enable-demuxer=mov,mp4,m4a,3gp,3g2,mj2,flv,rtsp,rtp,mpegts,h264,hevc,aac,matroska",
    "--enable-muxer=flv,rtsp,rtp,mpegts,null",
    "--enable-indev=dshow",
    "--enable-decoder=h264,mjpeg,rawvideo",
    "--enable-encoder=h264_mf",
    "--enable-parser=h264,hevc,aac,mpeg4video,mjpeg",
    "--enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,aac_adtstoasc"
)

$BuildJobs = if ($Arch.ToUpperInvariant() -in @("X86", "ARM64")) { 2 } else { 0 }

if ($Arch.ToUpperInvariant() -eq "ARM64") {
    $ConfigureArgs += "--enable-cross-compile"
}

$ConfigureCommand = ((@("$SourceUnix/configure") + $ConfigureArgs) | ForEach-Object { Quote-Sh $_ }) -join " "

New-Item -ItemType Directory -Force -Path $BuildDir, $Prefix, $IncludeDir, $LibDir | Out-Null
$env:MSYS2_PATH_TYPE = "inherit"

$BuildUnixQ = Quote-Sh $BuildUnix
$PrefixUnixQ = Quote-Sh $PrefixUnix
$MsvcBinUnixQ = Quote-Sh $MsvcBinUnix
$MsysMakeUnixQ = Quote-Sh $MsysMakeUnix
$RootIncludeQ = Quote-Sh "$RootUnix/include"
$RootLibQ = Quote-Sh "$RootUnix/lib"
$PrefixIncludeQ = Quote-Sh "$PrefixUnix/include/"
$PrefixLibQ = Quote-Sh "$PrefixUnix/lib/"
$PrefixBinQ = Quote-Sh "$PrefixUnix/bin/"
$BuildAvformatQ = Quote-Sh "$BuildUnix/libavformat/"
$BuildAvcodecQ = Quote-Sh "$BuildUnix/libavcodec/"
$BuildAvutilQ = Quote-Sh "$BuildUnix/libavutil/"
$BuildAvdeviceQ = Quote-Sh "$BuildUnix/libavdevice/"
$BuildSwscaleQ = Quote-Sh "$BuildUnix/libswscale/"
$BuildSwresampleQ = Quote-Sh "$BuildUnix/libswresample/"
$BuildJobsQ = Quote-Sh ([string]$BuildJobs)

$BuildScript = Join-Path $BuildDir "build_ffmpeg_windows.sh"
$ScriptLines = @(
    "#!/usr/bin/env bash",
    "set -euo pipefail",
    "export MSYSTEM=MSYS",
    "export CHERE_INVOKING=1",
    "export PATH=${MsvcBinUnixQ}:/usr/bin:`$PATH",
    "mkdir -p $BuildUnixQ $PrefixUnixQ $RootIncludeQ $RootLibQ",
    "cd $BuildUnixQ",
    "echo `"MSYS2 cl.exe: `$(command -v cl.exe || true)`"",
    "echo `"MSYS2 link.exe: `$(command -v link.exe || true)`"",
    "echo `"MSYS2 lib.exe: `$(command -v lib.exe || true)`"",
    "if [ ! -f config.mak ]; then $ConfigureCommand; fi",
    "echo `"FFmpeg selected build variables:`"",
    "/usr/bin/grep -E '^(CONFIG_SHARED|CONFIG_STATIC|CONFIG_AVFORMAT|CONFIG_AVCODEC|CONFIG_AVUTIL|CONFIG_AVDEVICE|CONFIG_SWSCALE|CONFIG_SWRESAMPLE|CC=|LD=|AR=|SLIBNAME|SLIBNAME_WITH_MAJOR|SLIBSUF|LIBSUF|SHFLAGS=)' ffbuild/config.mak || true",
    "MAKE_BIN=$MsysMakeUnixQ",
    "if [ ! -x `"`$MAKE_BIN`" ]; then echo `"MSYS make was not found or is not executable: `$MAKE_BIN`" >&2; exit 1; fi",
    "case `"`$MAKE_BIN`" in */mingw*/bin/make) echo `"Refusing MinGW make for FFmpeg MSVC build: `$MAKE_BIN`" >&2; echo `"Install MSYS make, usually from the MSYS2 base-devel package.`" >&2; exit 1 ;; esac",
    "MAX_JOBS=$BuildJobsQ",
    "if [ `"`$MAX_JOBS`" != `"0`" ]; then JOBS=`"`$MAX_JOBS`"; elif command -v nproc >/dev/null 2>&1; then JOBS=`$(nproc); else JOBS=2; fi",
    "echo `"Using make: `$MAKE_BIN`"",
    "`"`$MAKE_BIN`" -j`"`$JOBS`" libavutil/avutil.dll libswresample/swresample.dll libswscale/swscale.dll libavcodec/avcodec.dll libavformat/avformat.dll libavdevice/avdevice.dll",
    "`"`$MAKE_BIN`" install-libs install-headers",
    "echo `"Windows FFmpeg build-tree artifacts:`"",
    "/usr/bin/find . -maxdepth 3 -type f \( -name '*.dll' -o -name '*.lib' -o -name '*.def' -o -name '*.dll.a' \) -print | /usr/bin/sort",
    "echo `"Windows FFmpeg prefix artifacts:`"",
    "/usr/bin/find $PrefixUnixQ -maxdepth 4 -type f \( -name '*.dll' -o -name '*.lib' -o -name '*.def' -o -name '*.dll.a' \) -print | /usr/bin/sort",
    "/usr/bin/cp -a ${PrefixIncludeQ}* $RootIncludeQ",
    "for pattern in ${PrefixLibQ}*.lib ${PrefixBinQ}*.lib ${PrefixBinQ}*.dll ${BuildAvformatQ}*.lib ${BuildAvformatQ}*.dll ${BuildAvcodecQ}*.lib ${BuildAvcodecQ}*.dll ${BuildAvutilQ}*.lib ${BuildAvutilQ}*.dll ${BuildAvdeviceQ}*.lib ${BuildAvdeviceQ}*.dll ${BuildSwscaleQ}*.lib ${BuildSwscaleQ}*.dll ${BuildSwresampleQ}*.lib ${BuildSwresampleQ}*.dll; do if [ -e `"`$pattern`" ]; then /usr/bin/cp -a `"`$pattern`" $RootLibQ; fi; done",
    "/usr/bin/find $PrefixUnixQ $BuildUnixQ -maxdepth 4 -type f \( -name '*.dll' -o -name '*.lib' \) -exec /usr/bin/cp -a {} $RootLibQ \;",
    "echo `"Windows FFmpeg SDK lib directory:`"",
    "/usr/bin/find $RootLibQ -maxdepth 1 -type f | /usr/bin/sort"
)
$Script = $ScriptLines -join "`n"
[System.IO.File]::WriteAllText(
    $BuildScript,
    $Script + "`n",
    [System.Text.UTF8Encoding]::new($false)
)
$BuildScriptUnix = Convert-ToMsysPath $BuildScript
Write-Host "Generated FFmpeg build script: $BuildScript"
Write-Host "----- begin build_ffmpeg_windows.sh -----"
Write-Host $Script
Write-Host "----- end build_ffmpeg_windows.sh -----"

& $Bash $BuildScriptUnix
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$RequiredComponents = @("avformat", "avcodec", "avutil", "avdevice", "swscale", "swresample")

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

Copy-VCRuntimeDlls -RedistArch $VCRedistArch -DestinationDir $LibDir

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
