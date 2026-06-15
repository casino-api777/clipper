# Build, push, tag, and publish clip.exe to GitHub Releases.
# Prereq: gh auth login (as a user with push access to NexusGGR/clipper)
param(
    [string]$Version = "1.0.0",
    [string]$Tag = "v1.0.0"
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

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
"@

Write-Host "=== Creating GitHub release $Tag ==="
$existing = gh release view $Tag 2>$null
if ($LASTEXITCODE -eq 0) {
    gh release upload $Tag clip.exe --clobber
    Write-Host "Updated assets on existing release $Tag"
} else {
    gh release create $Tag clip.exe --title "Clipper $Version" --notes $notes
}

$url = gh release view $Tag --json url -q .url
Write-Host "Done: $url"
