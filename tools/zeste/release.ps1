# Zeste release: changelog, tag, build, checks, zip, smoke test, publish on GitLab and GitHub.
#
#   powershell -ExecutionPolicy Bypass -File tools\zeste\release.ps1 -Version 0.5.2
#   ... -Version 0.5.2-pre1          pre-release (never offered by the update check)
#   ... -Version 0.5.2 -DryRun       everything up to the zip and the smoke test, nothing pushed
#   ... -Version 0.5.2 -NotesFile n.md   release notes from a file instead of the changelog
#
# The order matters: the version is read from the tag when CMake configures, so the tag comes
# before the build. The zip never holds a `user` folder or ROMs.

param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$NotesFile = "",
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path "$PSScriptRoot\..\..").Path
Set-Location $Root
$Tag = "v$Version"
$PreRelease = $Version -match "-pre\d+$"
$Name = "Zeste-$Version-windows-x64"
$Glab = "$env:LOCALAPPDATA\Programs\glab\glab.exe"
$Gh = "C:\Program Files\GitHub CLI\gh.exe"
$VcVars = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

function Step($text) { Write-Host "`n== $text" -ForegroundColor Cyan }
function Fail($text) { throw "release: $text" }
function Git { & git @args; if ($LASTEXITCODE -ne 0) { Fail "git $args failed" } }

if ($Version -notmatch "^\d+\.\d+\.\d+(-pre\d+)?$") { Fail "version must look like 1.2.3 or 1.2.3-pre1" }

Step "Checks"
if ((git rev-parse --abbrev-ref HEAD) -ne "main") { Fail "not on main" }
if (git status --porcelain) { Fail "the working tree has changes: commit or stash them first" }
Git fetch -q origin
if (-not $DryRun -and (git rev-parse HEAD) -ne (git rev-parse origin/main)) { Fail "main and origin/main differ: push or pull first" }
if (git tag -l $Tag) { Fail "tag $Tag already exists" }
if (-not $DryRun) {
    if (-not (Test-Path $Glab)) { Fail "glab not found at $Glab" }
    if (-not (Test-Path $Gh)) { Fail "gh not found at $Gh" }
}

Step "Changelog"
$changelog = Get-Content CHANGELOG.md -Raw
$m = [regex]::Match($changelog, "(?s)## Unreleased\r?\n(.*?)(?=\r?\n## )")
$unreleased = if ($m.Success) { $m.Groups[1].Value.Trim() } else { "" }
if (-not $unreleased -and -not $NotesFile) { Fail "CHANGELOG.md has nothing under Unreleased (or pass -NotesFile)" }
$date = Get-Date -Format "yyyy-MM-dd"
if ($unreleased) {
    $changelog = $changelog.Remove($m.Index, $m.Length).Insert($m.Index,
        "## Unreleased`n`n## $Version ($date)`n`n$unreleased`n")
}
$notesPath = Join-Path $env:TEMP "zeste-release-notes-$Version.md"
if ($NotesFile) { Copy-Item $NotesFile $notesPath -Force } else { Set-Content $notesPath $unreleased -NoNewline }
Write-Host (Get-Content $notesPath -Raw)
if ($unreleased -and -not $DryRun) {
    [IO.File]::WriteAllText("$Root\CHANGELOG.md", $changelog)
    Git add CHANGELOG.md
    Git commit -q -m "Changelog: $Version"
    Git push -q origin main
}

Step "Tag $Tag"
if (-not $DryRun) {
    Git tag $Tag
    Git push -q origin $Tag
} else {
    Git tag $Tag  # local only, removed at the end of a dry run
}

try {
    Step "Build and bundle"
    cmd /c "`"$VcVars`" >NUL && cmake --build build && cmake --build build --target bundle"
    if ($LASTEXITCODE -ne 0) { Fail "build failed" }

    Step "Version and bundle checks"
    $built = "$Root\build\bin\Release\zeste.exe"
    $bundled = "$Root\build\bundle\zeste.exe"
    if ((Get-FileHash $built).Hash -ne (Get-FileHash $bundled).Hash) { Fail "build\bundle\zeste.exe is not the exe just built" }
    $reported = (& $bundled --version 2>&1 | Out-String).Trim()
    if ($reported -notmatch [regex]::Escape($Version) -or $reported -match "-dev") { Fail "the bundle says '$reported', expected $Version" }
    Write-Host "zeste.exe --version: $reported"

    Step "Zip"
    $stage = Join-Path $env:TEMP "zeste-release\$Name"
    if (Test-Path "$env:TEMP\zeste-release") { cmd /c "rmdir /s /q `"$env:TEMP\zeste-release`"" }
    if (Test-Path "$Root\build\bundle\user") { cmd /c "rmdir /s /q `"$Root\build\bundle\user`"" }
    New-Item -ItemType Directory -Force $stage | Out-Null
    Copy-Item "$Root\build\bundle\*" $stage -Recurse
    Copy-Item "$Root\README.md", "$Root\license.txt" $stage
    Get-ChildItem $stage -Recurse -Directory -Filter "__pycache__" | ForEach-Object { cmd /c "rmdir /s /q `"$($_.FullName)`"" }
    New-Item -ItemType Directory -Force "$Root\build\release" | Out-Null
    $zip = "$Root\build\release\$Name.zip"
    if (Test-Path $zip) { [IO.File]::Delete($zip) }
    Compress-Archive -Path $stage -DestinationPath $zip
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $entries = [IO.Compression.ZipFile]::OpenRead($zip).Entries | ForEach-Object { $_.FullName -replace "\\", "/" }
    $bad = $entries | Where-Object { $_ -match "/user/" -or $_ -match "\.(3dsx|cia|cxi|cci|3ds|elf|app)$" }
    if ($bad) { Fail "the zip holds user data or ROMs: $($bad -join ', ')" }
    Write-Host ("{0} ({1:N1} MB, {2} files)" -f $zip, ((Get-Item $zip).Length / 1MB), $entries.Count)

    Step "Smoke test from an extracted copy"
    $check = "$Root\build\relcheck"
    if (Test-Path $check) { cmd /c "rmdir /s /q `"$check`"" }
    Expand-Archive $zip $check
    $exe = "$check\$Name\zeste.exe"
    $rom = "$Root\tools\zeste\testrom\zeste-tests.cia"
    if (Test-Path $rom) {
        $out = & $exe --headless --strict --quit-after 20s $rom 2>&1 | Out-String
        if ($LASTEXITCODE -ne 0) { Write-Host $out; Fail "the smoke test exited with $LASTEXITCODE" }
        Write-Host "test ROM: exit code 0"
    } else {
        Write-Host "tools\zeste\testrom\zeste-tests.cia not found: only --version checked" -ForegroundColor Yellow
        & $exe --version | Out-Null
        if ($LASTEXITCODE -ne 0) { Fail "the extracted zeste.exe does not start" }
    }

    if ($DryRun) {
        Step "Dry run: nothing published ($zip)"
        return
    }

    Step "Publish"
    & $Glab release create $Tag $zip --name "Zeste $Version" --notes-file $notesPath -R Synaelle/zeste
    if ($LASTEXITCODE -ne 0) { Fail "GitLab release failed" }
    $ghArgs = @("release", "create", $Tag, $zip, "--repo", "Synaelle/zeste", "--title", "Zeste $Version", "--notes-file", $notesPath)
    $ghArgs += if ($PreRelease) { "--prerelease" } else { "--latest" }
    & $Gh @ghArgs
    if ($LASTEXITCODE -ne 0) { Fail "GitHub release failed (the GitLab one is out)" }
    Step "Zeste $Version released"
} finally {
    if ($DryRun) { git tag -d $Tag | Out-Null }
}
