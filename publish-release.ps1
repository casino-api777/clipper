# Build, push, tag, and publish clip.exe to GitHub Releases.
# Prereq: gh auth login (as a user with push access to NexusGGR/clipper)
param(
    [string]$Version = "",
    [string]$Tag = "v1.0.0"
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

function Resolve-Version {
    param(
        [string]$InputVersion,
        [string]$InputTag
    )

    if ($InputVersion -and $InputVersion.Trim().Length -gt 0) {
        return $InputVersion.Trim()
    }

    if ($InputTag -match '^v?(.+)$') {
        return $Matches[1]
    }

    throw "Unable to determine version from inputs."
}

function Get-VersionParts {
    param([string]$VersionString)

    $parts = $VersionString.Split('.')
    if ($parts.Count -lt 3 -or $parts.Count -gt 4) {
        throw "Version '$VersionString' must be 3 or 4 numeric parts, e.g. 1.2.3 or 1.2.3.4"
    }

    $numbers = @()
    foreach ($p in $parts) {
        if ($p -notmatch '^\d+$') {
            throw "Version '$VersionString' contains non-numeric part '$p'"
        }
        $numbers += [int]$p
    }
    while ($numbers.Count -lt 4) { $numbers += 0 }
    return $numbers
}

function Update-ClipResourceVersion {
    param([string]$RcPath, [string]$VersionString)

    $v = Get-VersionParts -VersionString $VersionString
    $numeric = "$($v[0]),$($v[1]),$($v[2]),$($v[3])"
    $stringVersion = "$($v[0]).$($v[1]).$($v[2]).$($v[3])"

    $content = Get-Content -Raw -Path $RcPath
    $content = [regex]::Replace($content, '(?m)^FILEVERSION\s+[0-9,\s]+$', "FILEVERSION $numeric")
    $content = [regex]::Replace($content, '(?m)^PRODUCTVERSION\s+[0-9,\s]+$', "PRODUCTVERSION $numeric")
    $content = [regex]::Replace($content, '(?m)^(\s*VALUE\s+"ProductVersion",\s*")[^"]*(".*)$', "`$1$stringVersion`$2")
    Set-Content -Path $RcPath -Value $content -NoNewline
}

$Version = Resolve-Version -InputVersion $Version -InputTag $Tag
$rcPath = Join-Path $PSScriptRoot "clip.rc"
Update-ClipResourceVersion -RcPath $rcPath -VersionString $Version
$env:CLIP_VERSION = $Version

if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
    Write-Error "GitHub CLI (gh) not found. Install from https://cli.github.com/"
}

$auth = gh auth status 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "Not logged in to GitHub. Run: gh auth login"
    gh auth login
}

Write-Host "=== Building clip.exe ==="
& .\build-clip.bat
if (-not (Test-Path "clip.exe")) {
    Write-Error "Build failed: clip.exe not found"
}

Write-Host "=== Pushing source and tag ==="
git push -u origin master
git push origin $Tag

$notes = @"
Clipper $Version

Windows utility that masks keystrokes in password fields and terminals with random decoy input.

**Requirements:** Windows 10+, administrator on first run.

**Usage:** Run ``clip.exe`` or ``clip.exe --all`` for all text fields.

## What's new
- Added service startup + recovery configuration (7-second restart actions).
- Added automatic EXE version stamping during build from release/tag version.
- Improved release automation so build version is forced from release version.
"@

Write-Host "=== Creating GitHub release $Tag ==="
$existing = gh release view $Tag 2>$null
if ($LASTEXITCODE -eq 0) {
    gh release upload $Tag clip.exe --clobber
    gh release edit $Tag --title "Clipper $Version" --notes $notes
    Write-Host "Updated assets on existing release $Tag"
} else {
    gh release create $Tag clip.exe --title "Clipper $Version" --notes $notes
}

$url = gh release view $Tag --json url -q .url
Write-Host "Done: $url"
