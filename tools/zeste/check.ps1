# Zeste build check: the unit tests, then the test ROM (tools/zeste/testrom, kept out of git)
# headless in strict mode, with the lines its output must contain. Run by build_windows.cmd after
# every build, or alone:
#
#   powershell -ExecutionPolicy Bypass -File tools\zeste\check.ps1 [-Exe path\to\zeste.exe]

param([string]$Exe = "")

$ErrorActionPreference = "Continue" # native tools write to stderr
$Root = (Resolve-Path "$PSScriptRoot\..\..").Path
if (-not $Exe) { $Exe = "$Root\build\bin\Release\zeste.exe" }
$Tests = Join-Path (Split-Path $Exe) "tests.exe"
$Rom = "$Root\tools\zeste\testrom\zeste-tests.cia"
$failed = @()

if (Test-Path $Tests) {
    $out = & $Tests 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { Write-Host $out; $failed += "unit tests" }
    else { Write-Host ("check: unit tests ok ({0})" -f (($out -split "`n" | Select-String "test cases:") -join "").Trim()) }
}

if (-not (Test-Path $Rom)) {
    Write-Host "check: no test ROM ($Rom), skipped" -ForegroundColor Yellow
} else {
    # Waits for the line that says the ROM is ready (no guessed frame numbers), then X: a write
    # to NULL, which the ROM's exception handler answers right away with "[X] PASS".
    $script = Join-Path $env:TEMP "zeste-check.zs"
    Set-Content $script @(
        "@1 wait-log timeout=120s C3D_GetDrawingTime",
        "@2 press X 5",
        "@120 quit 0"
    )
    $out = & $Exe --headless --strict --boot-cia $Rom --script $script 2>&1 | Out-String
    $code = $LASTEXITCODE
    $folder = [regex]::Match($out, "Zeste run folder: (\S+)").Groups[1].Value
    $game = if ($folder -and (Test-Path "$folder\game_output.txt")) { Get-Content "$folder\game_output.txt" -Raw } else { "" }
    $expected = @(
        "Zeste test ROM",
        "svcGetProcessorID (core 1 thread) = 1",          # kernel_fixes
        "getaddrinfo conntest.nintendowifi.net -> failed", # Nintendo's servers are never reached
        "gethostbyname account.nintendo.net -> failed",
        "C3D_GetDrawingTime after 60 frames",
        "[X] exception handler ran",                       # cpu_exceptions
        "[X] PASS"
    )
    $missing = $expected | Where-Object { -not $game.Contains($_) }
    if ($code -ne 0) { $failed += "test ROM exit code $code" }
    if ($missing) { $failed += "test ROM output lacks: " + ($missing -join " | ") }
    if (-not $folder) { $failed += "no run folder in the output" }
    if ($code -eq 0 -and -not $missing) { Write-Host "check: test ROM ok ($($expected.Count) lines, $folder)" }
    elseif ($folder) { Write-Host "check: see $folder (summary.md, game_output.txt)" }
}

if ($failed) {
    Write-Host ("check FAILED: " + ($failed -join "; ")) -ForegroundColor Red
    exit 1
}
Write-Host "check: all good" -ForegroundColor Green
