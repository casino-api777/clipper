param(
    [string]$Version = ""
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

if (-not $Version -or $Version.Trim().Length -eq 0) {
    if ($env:CLIP_VERSION -and $env:CLIP_VERSION.Trim().Length -gt 0) {
        $Version = $env:CLIP_VERSION.Trim()
    }
}

if (-not $Version -or $Version.Trim().Length -eq 0) {
    $tag = git describe --tags --abbrev=0 2>$null
    if ($LASTEXITCODE -eq 0 -and $tag) {
        $Version = $tag.Trim()
    }
}

if (-not $Version -or $Version.Trim().Length -eq 0) {
    $Version = "1.0.0"
}

if ($Version -match '^v(.+)$') {
    $Version = $Matches[1]
}

if ($Version -notmatch '^(\d+)\.(\d+)\.(\d+)(?:\.(\d+))?$') {
    throw "Version '$Version' must look like 1.2.3 or 1.2.3.4"
}

$major = [int]$Matches[1]
$minor = [int]$Matches[2]
$patch = [int]$Matches[3]
$build = if ($Matches[4]) { [int]$Matches[4] } else { 0 }
$versionString = "$major.$minor.$patch.$build"

$header = @"
#pragma once

#define CLIP_VER_MAJOR $major
#define CLIP_VER_MINOR $minor
#define CLIP_VER_PATCH $patch
#define CLIP_VER_BUILD $build

#define CLIP_VER_NUMERIC CLIP_VER_MAJOR,CLIP_VER_MINOR,CLIP_VER_PATCH,CLIP_VER_BUILD
#define CLIP_VER_STRING "$versionString"
"@

Set-Content -Path (Join-Path $PSScriptRoot "clip_version.h") -Value $header -NoNewline
Write-Host "Generated clip_version.h for version $versionString"
