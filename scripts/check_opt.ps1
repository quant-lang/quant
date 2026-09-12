# check_opt.ps1 - Differential test of the IR optimizer.
#
# Every runnable test (not *_err, not a library module) is compiled at a
# reference level (-O0 by default) and at each level under test (-O1 -O2 -O3
# by default). The produced executables are run and their stdout + exit code
# must be identical to the reference. This catches miscompilations introduced
# by any optimization pass, not just compile errors.
#
# Overrides:
#   QU         - compiler binary (default: build/bin/qu.exe)
#   QU_REF     - reference level (default: -O0)
#   QU_LEVELS  - levels under test, space separated (default: "-O1 -O2 -O3")
#   QU_TIMEOUT - per-run timeout in seconds (default: 20)
#
# Exit code: 0 if every level matches the reference on every test, 1 otherwise.

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path $PSScriptRoot -Parent
$TestsDir = Join-Path $RepoRoot "tests"

if ($env:QU) { $Qu = $env:QU } else { $Qu = Join-Path $RepoRoot "build/bin/qu.exe" }
if (-not (Test-Path $Qu)) {
    Write-Error "FATAL: compiler not found at $Qu"
    exit 1
}
$Ref = if ($env:QU_REF) { $env:QU_REF } else { "-O0" }
$Levels = if ($env:QU_LEVELS) { $env:QU_LEVELS -split '\s+' | Where-Object { $_ } } else { @("-O1", "-O2", "-O3") }
$Timeout = if ($env:QU_TIMEOUT) { [int]$env:QU_TIMEOUT } else { 20 }

$Work = Join-Path ([System.IO.Path]::GetTempPath()) ("qu_check_opt_" + [System.Diagnostics.Process]::GetCurrentProcess().Id)
New-Item -ItemType Directory -Force $Work | Out-Null

# Tests that cannot be driven non-interactively or are not for the host target.
$Skip = @("stdin_read", "zp_hello")
# Tests whose stdout is nondeterministic (prints OS handles): compare exit codes only.
$ExitCodeOnly = @("file_write")

function Invoke-Capture([string]$exe, [string[]]$argv, [string]$cwd) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = ($argv | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $psi.WorkingDirectory = $cwd
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.RedirectStandardInput = $true
    $p = [System.Diagnostics.Process]::Start($psi)
    $p.StandardInput.Close()
    $outTask = $p.StandardOutput.ReadToEndAsync()
    $errTask = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit($Timeout * 1000)) {
        try { $p.Kill() } catch {}
        return @{ code = -999; out = "<timeout>" }
    }
    $p.WaitForExit()
    return @{ code = $p.ExitCode; out = $outTask.Result }
}

$pass = 0; $fail = 0; $skipped = 0; $compileFail = 0
$failList = @()

$files = Get-ChildItem -Path $TestsDir -Filter "*.qu" -Recurse | Sort-Object FullName
foreach ($file in $files) {
    $base = [System.IO.Path]::GetFileNameWithoutExtension($file.Name)
    if ($base -match '_err$') { continue }
    if (Get-Content $file.FullName -First 5 | Where-Object { $_ -match '^module ' }) { continue }
    if ($Skip -contains $base) { $skipped++; continue }

    $rel = $file.FullName.Substring($TestsDir.Length + 1)
    $results = @{}
    $ok = $true
    foreach ($lvl in @($Ref) + $Levels) {
        $exe = Join-Path $Work ("$base" + $lvl.TrimStart('-') + ".exe")
        # Compile from the repo root: tests load sibling modules by path (tests::lib_*).
        $c = Invoke-Capture $Qu @($file.FullName, $lvl, "-o", $exe) $RepoRoot
        if ($c.code -ne 0 -or -not (Test-Path $exe)) {
            $compileFail++
            $failList += "$rel [$lvl]: compile failed"
            $ok = $false
            break
        }
        $results[$lvl] = Invoke-Capture $exe @() $Work
    }
    if (-not $ok) { $fail++; continue }

    foreach ($lvl in $Levels) {
        $r = $results[$Ref]; $t = $results[$lvl]
        $outDiffers = ($r.out -ne $t.out) -and -not ($ExitCodeOnly -contains $base)
        if ($r.code -ne $t.code -or $outDiffers) {
            $failList += "$rel [$lvl vs $Ref]: exit $($t.code) vs $($r.code)"
            if ($outDiffers) {
                $failList += "    stdout differs:"
                $failList += "    ref: " + ($r.out -replace "`r?`n", "\n")
                $failList += "    $lvl" + ": " + ($t.out -replace "`r?`n", "\n")
            }
            $ok = $false
        }
    }
    if ($ok) { $pass++ } else { $fail++ }
}

Remove-Item -Recurse -Force $Work -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "========================================"
Write-Host "  Quant Optimizer Differential Report"
Write-Host "========================================"
Write-Host ""
Write-Host "  Reference:       $Ref"
Write-Host "  Levels:          $($Levels -join ' ')"
Write-Host "  ----------------------------------------"
Write-Host "  Matching:        $pass"
Write-Host "  Mismatching:     $fail"
Write-Host "  Compile failed:  $compileFail"
Write-Host "  Skipped:         $skipped"
Write-Host "========================================"

if ($failList.Count -gt 0) {
    Write-Host ""
    Write-Host "Failures:"
    foreach ($f in $failList) { Write-Host "  $f" }
    Write-Host ""
    Write-Host "Optimizer output differs from the reference."
    exit 1
}
Write-Host ""
Write-Host "All levels match the reference."
exit 0
