# check_errors.ps1 - Run all Quant tests and verify expected error status.
#
# Convention:
#   - Tests ending with _err are EXPECTED to fail (compile error).
#   - All other tests are expected to succeed.
#   - Files containing a "module" declaration are library modules and skipped.
#
# Exit code: 0 if all tests match expectations, 1 otherwise.

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path $PSScriptRoot -Parent
$TestsDir = Join-Path $RepoRoot "tests"

# Overrides:
#   QU       - path to the compiler binary (default: build/bin/qu.exe)
#   QU_FLAGS - extra compiler flags, e.g. "-O0" or "-O3" (default: none)
if ($env:QU) {
    $Qu = $env:QU
} else {
    $Qu = Join-Path $RepoRoot "build/bin/qu.exe"
    if (-not (Test-Path $Qu)) {
        # Try Linux-style path (e.g. running from WSL)
        $Qu = Join-Path $RepoRoot "build/bin/qu"
    }
}
if (-not (Test-Path $Qu)) {
    Write-Error "FATAL: compiler not found at $Qu"
    exit 1
}
$QuFlags = @()
if ($env:QU_FLAGS) { $QuFlags = $env:QU_FLAGS -split '\s+' | Where-Object { $_ } }

$pass = 0
$expectedErr = 0
$unexpectedErr = 0
$unexpectedOk = 0
$skipped = 0

$unexpectedErrList = @()
$unexpectedOkList = @()

$files = Get-ChildItem -Path $TestsDir -Filter "*.qu" -Recurse | Sort-Object FullName

foreach ($file in $files) {
    # Skip library modules
    $firstDecl = Get-Content $file.FullName -First 5 | Where-Object { $_ -match '^module ' }
    if ($firstDecl) {
        $skipped++
        continue
    }

    $relPath = $file.FullName.Substring($TestsDir.Length + 1)
    $baseName = [System.IO.Path]::GetFileNameWithoutExtension($file.Name)

    $args = @($file.FullName) + $QuFlags
    $proc = Start-Process -FilePath $Qu -ArgumentList $args -NoNewWindow -Wait -PassThru -RedirectStandardOutput "$env:TEMP\qu_out.tmp" -RedirectStandardError "$env:TEMP\qu_err.tmp"

    if ($proc.ExitCode -eq 0) {
        if ($baseName -match '_err$') {
            $unexpectedOk++
            $unexpectedOkList += $relPath
        } else {
            $pass++
        }
    } else {
        if ($baseName -match '_err$') {
            $expectedErr++
        } else {
            $unexpectedErr++
            $unexpectedErrList += $relPath
        }
    }
}

$total = $pass + $expectedErr + $unexpectedErr + $unexpectedOk

Write-Host ""
Write-Host "========================================"
Write-Host "  Quant Test Report"
Write-Host "========================================"
Write-Host ""
Write-Host "  Total tests run:   $total"
Write-Host "  Skipped (modules): $skipped"
Write-Host "  ----------------------------------------"
Write-Host "  Passed:            $pass"
Write-Host "  Expected errors:   $expectedErr"
Write-Host "  UNEXPECTED errors: $unexpectedErr"
Write-Host "  UNEXPECTED passes: $unexpectedOk"
Write-Host "========================================"

if ($unexpectedErrList.Count -gt 0) {
    Write-Host ""
    Write-Host "Tests that FAIL but are NOT marked _err (need renaming):"
    foreach ($f in $unexpectedErrList) {
        Write-Host "  - $f"
    }
}

if ($unexpectedOkList.Count -gt 0) {
    Write-Host ""
    Write-Host "Tests marked _err but PASSING (remove _err or fix test):"
    foreach ($f in $unexpectedOkList) {
        Write-Host "  - $f"
    }
}

if ($unexpectedErr -eq 0 -and $unexpectedOk -eq 0) {
    Write-Host ""
    Write-Host "All tests match expectations."
    exit 0
} else {
    Write-Host ""
    Write-Host "Some tests do not match expectations."
    exit 1
}
