# bench_opt.ps1 - End-to-end optimizer benchmark (compile time + runtime).
#
# For every input program and every optimization level the script compiles
# the program K times with the real compiler (reporting the minimum
# "Compilation took" time), runs the produced executable N times (reporting
# the mean wall time) and checks that stdout is identical across levels.
#
# Usage:
#   scripts\bench_opt.ps1 [file.qu ...]
#
# Overrides:
#   QU          - compiler binary (default: build/bin/qu.exe)
#   QU_LEVELS   - levels, space separated (default: "-O0 -O1 -O2 -O3")
#   QU_COMPILES - compilations per level (default: 5)
#   QU_RUNS     - executions per level (default: 20)
#
# Default inputs: tests/bench_loop_fold.qu tests/loop_fold.qu tests/if_switch_fold.qu

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path $PSScriptRoot -Parent
if ($env:QU) { $Qu = $env:QU } else { $Qu = Join-Path $RepoRoot "build/bin/qu.exe" }
if (-not (Test-Path $Qu)) {
    Write-Error "FATAL: compiler not found at $Qu"
    exit 1
}
$Levels = if ($env:QU_LEVELS) { $env:QU_LEVELS -split '\s+' | Where-Object { $_ } } else { @("-O0", "-O1", "-O2", "-O3") }
$Compiles = if ($env:QU_COMPILES) { [int]$env:QU_COMPILES } else { 5 }
$Runs = if ($env:QU_RUNS) { [int]$env:QU_RUNS } else { 20 }

$Inputs = @($args)
if ($Inputs.Count -eq 0) {
    $Inputs = @("tests/bench_loop_fold.qu", "tests/loop_fold.qu", "tests/if_switch_fold.qu")
}

$Work = Join-Path ([System.IO.Path]::GetTempPath()) ("qu_bench_" + [System.Diagnostics.Process]::GetCurrentProcess().Id)
New-Item -ItemType Directory -Force $Work | Out-Null

function Invoke-Capture([string]$exe, [string[]]$argv, [string]$cwd) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = ($argv | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $psi.WorkingDirectory = $cwd
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $p = [System.Diagnostics.Process]::Start($psi)
    $out = $p.StandardOutput.ReadToEnd()
    $null = $p.StandardError.ReadToEnd()
    $p.WaitForExit()
    $sw.Stop()
    return @{ code = $p.ExitCode; out = $out; ms = $sw.Elapsed.TotalMilliseconds }
}

$failed = $false
foreach ($inputRel in $Inputs) {
    $input = if ([System.IO.Path]::IsPathRooted($inputRel)) { $inputRel } else { Join-Path $RepoRoot $inputRel }
    $base = [System.IO.Path]::GetFileNameWithoutExtension($input)
    Write-Host ""
    Write-Host "### $inputRel"
    Write-Host ""
    Write-Host "| level | compile, ms (min of $Compiles) | run, ms (mean of $Runs) | exit | output |"
    Write-Host "|---|---|---|---|---|"

    $refOut = $null
    foreach ($lvl in $Levels) {
        $exe = Join-Path $Work ("$base" + $lvl.TrimStart('-') + ".exe")
        $compileMs = [double]::PositiveInfinity
        $ok = $true
        for ($i = 0; $i -lt $Compiles; $i++) {
            $c = Invoke-Capture $Qu @($input, $lvl, "--time", "-o", $exe) $RepoRoot
            if ($c.code -ne 0) { $ok = $false; break }
            if ($c.out -match 'Compilation took:\s*([0-9.]+)\s*ms') {
                $t = [double]$Matches[1]
                if ($t -lt $compileMs) { $compileMs = $t }
            }
        }
        if (-not $ok) {
            Write-Host "| $lvl | compile failed | | | |"
            $failed = $true
            continue
        }

        $total = 0.0
        $code = 0
        $out = ""
        for ($i = 0; $i -lt $Runs; $i++) {
            $r = Invoke-Capture $exe @() $Work
            $total += $r.ms
            $code = $r.code
            $out = $r.out
        }
        $meanMs = $total / $Runs
        $outFlat = ($out.Trim() -replace "`r?`n", " ")
        if ($outFlat.Length -gt 60) { $outFlat = $outFlat.Substring(0, 57) + "..." }

        $match = ""
        if ($null -eq $refOut) { $refOut = $out } elseif ($refOut -ne $out) { $match = " **DIFFERS**"; $failed = $true }

        $inv = [System.Globalization.CultureInfo]::InvariantCulture
        Write-Host ("| {0} | {1} | {2} | {3} | {4}{5} |" -f $lvl, $compileMs.ToString("0.0", $inv), $meanMs.ToString("0.00", $inv), $code, $outFlat, $match)
    }
}

Remove-Item -Recurse -Force $Work -ErrorAction SilentlyContinue
if ($failed) { exit 1 }
exit 0
