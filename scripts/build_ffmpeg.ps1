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

foreach ($tool in @("cl.exe", "link.exe", "lib.exe")) {
    if (!(Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "$tool was not found. Run this script from a Visual Studio Developer shell, or use the GitHub Actions workflow."
    }
}

$BashCandidates = @(
    "$env:MSYS2_ROOT\usr\bin\bash.exe",
    "C:\msys64\usr\bin\bash.exe",
    "C:\msys2\usr\bin\bash.exe"
) | Where-Object { $_ -and (Test-Path $_) }

if ($BashCandidates.Count -eq 0) {
    $bashCommand = Get-Command "bash.exe" -ErrorAction SilentlyContinue
    if ($bashCommand) {
        $BashCandidates = @($bashCommand.Source)
    }
}

if ($BashCandidates.Count -eq 0) {
    throw "MSYS2 bash.exe was not found. Install MSYS2 or run the GitHub Actions workflow."
}

$Bash = $BashCandidates[0]

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
    "--disable-postproc",
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
$ConfigureHelp = (& $Bash -lc "$(Quote-Sh "$SourceUnix/configure") --help" 2>$null) -join "`n"
$SupportedConfigureArgs = @()
foreach ($Arg in $ConfigureArgs) {
    $Option = ($Arg -split "=", 2)[0]
    if ($Option -in @("--prefix", "--toolchain", "--target-os", "--arch") -or $ConfigureHelp.Contains($Option)) {
        $SupportedConfigureArgs += $Arg
    } else {
        Write-Warning "Skipping unsupported FFmpeg configure option: $Arg"
    }
}
$ConfigureCommand = ((@("$SourceUnix/configure") + $SupportedConfigureArgs) | ForEach-Object { Quote-Sh $_ }) -join " "

New-Item -ItemType Directory -Force -Path $BuildDir, $Prefix, $IncludeDir, $LibDir | Out-Null
$env:MSYS2_PATH_TYPE = "inherit"

$BuildUnixQ = Quote-Sh $BuildUnix
$PrefixUnixQ = Quote-Sh $PrefixUnix
$RootIncludeQ = Quote-Sh "$RootUnix/include"
$RootLibQ = Quote-Sh "$RootUnix/lib"
$PrefixIncludeQ = Quote-Sh "$PrefixUnix/include/"
$PrefixLibQ = Quote-Sh "$PrefixUnix/lib/"
$PrefixBinQ = Quote-Sh "$PrefixUnix/bin/"

$Script = @"
set -euo pipefail
mkdir -p $BuildUnixQ $PrefixUnixQ $RootIncludeQ $RootLibQ
cd $BuildUnixQ
if [ ! -f config.mak ]; then
  $ConfigureCommand
fi
make -j`$(nproc)
make install
cp -a ${PrefixIncludeQ}* $RootIncludeQ
cp -a ${PrefixLibQ}*.lib $RootLibQ
cp -a ${PrefixBinQ}*.dll $RootLibQ
"@

& $Bash -lc $Script
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

Write-Host "FFmpeg SDK built into:"
Write-Host "  $IncludeDir"
Write-Host "  $LibDir"
