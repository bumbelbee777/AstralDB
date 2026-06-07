# Load warehouse once per benchmark via --time-sql-setup; print --time-sql timings.
param(
    [string]$Astral = (Join-Path $PSScriptRoot "..\build\astraldb.exe"),
    [int]$Rows = 100000,
    [int]$TimeSqlRuns = 1,
    [string[]]$Vm = @("AUTO"),
    [int[]]$Opt = @(2, 3, 4),
    [switch]$Verify,
    [switch]$Precomputed,
    [switch]$ProfileHeavy,
    [switch]$TwoMillion
)

$ErrorActionPreference = "Stop"
$Repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$BenchDir = Join-Path $Repo "examples\benchmarks"
$SetupTpl = Join-Path $BenchDir "_bench_setup_small.sql"
$Astral = (Resolve-Path $Astral).Path
if (-not (Test-Path $Astral)) { throw "Missing $Astral (build Release first)." }

if ($TwoMillion) {
    $Rows = 2000000
    $TimeSqlRuns = [Math]::Max($TimeSqlRuns, 3)
    $Precomputed = $true
    $Verify = $true
    $Opt = @(4)
}

$tmpdir = Join-Path $env:TEMP "astral_storage_bench"
if (Test-Path $tmpdir) { Remove-Item -Recurse -Force $tmpdir -ErrorAction SilentlyContinue }
New-Item -ItemType Directory -Force -Path $tmpdir | Out-Null
$setupFile = Join-Path $tmpdir "setup.sql"
$setupText = (Get-Content $SetupTpl -Raw) -replace 'BULK\s+\d+', "BULK $Rows"
Set-Content -Path $setupFile -Value $setupText -Encoding utf8

$profileJson = Join-Path $tmpdir "bench_heavy_2m.json"
$summaryJson = Join-Path $tmpdir "bench_heavy_2m_summary.json"

$env:ASTRALDB_VERIFY_FAST_PATH = if ($Verify) { "1" } else { "0" }
$env:ASTRALDB_USE_PRECOMPUTED = if ($Precomputed) { "1" } else { "0" }
if ($ProfileHeavy -or $TwoMillion) {
    $env:ASTRALDB_PROFILE_OUTPUT = $profileJson
    $env:ASTRALDB_PROFILE_NAME = "bench_heavy_2m"
} else {
    Remove-Item Env:ASTRALDB_PROFILE_OUTPUT -ErrorAction SilentlyContinue
    Remove-Item Env:ASTRALDB_PROFILE_NAME -ErrorAction SilentlyContinue
}
if ($TwoMillion) {
    Remove-Item Env:ASTRALDB_DISABLE_BULK_SPILL -ErrorAction SilentlyContinue
}

function Get-ExecuteMsFromOutput {
    param([object]$Out)
    $line = ($Out | Select-String "\[time-sql\]" | Select-Object -Last 1).Line
    if (-not $line) { return $null }
    if ($line -match 'execute_median_ms=(\d+(?:\.\d+)?)') { return [double]$Matches[1] }
    if ($line -match 'execute_ms=(\d+(?:\.\d+)?)') { return [double]$Matches[1] }
    return $null
}

function Get-TimeSqlStatsFromOutput {
    param([object]$Out)
    $line = ($Out | Select-String "\[time-sql\]" | Select-Object -Last 1).Line
    if (-not $line) { return $null }
    $stats = @{ execute_ms = Get-ExecuteMsFromOutput $Out }
    if ($line -match 'result_rows=(\d+)') { $stats.result_rows = [int]$Matches[1] }
    if ($line -match 'fast_path_flags=(\d+)') { $stats.fast_path_flags = [int]$Matches[1] }
    if ($line -match 'execute_min_ms=([\d.]+).*execute_max_ms=([\d.]+)') {
        $stats.execute_min_ms = [double]$Matches[1]
        $stats.execute_max_ms = [double]$Matches[2]
    }
    return $stats
}

function Invoke-TimedSql {
    param([string]$VmName, [int]$OptLevel, [string]$SetupPath, [string]$BenchPath, [bool]$UsePrecomputed = $true)
    $env:ASTRALDB_VM = $VmName
    $env:ASTRALDB_OPT_LEVEL = "$OptLevel"
    $env:ASTRALDB_USE_PRECOMPUTED = if ($UsePrecomputed) { "1" } else { "0" }
    $args = @("-m", "-O$OptLevel", "--time-sql-setup", $SetupPath, "--time-sql", $BenchPath)
    if ($TimeSqlRuns -gt 1) {
        $args += @("--time-sql-runs", "$TimeSqlRuns")
    }
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $out = & $Astral @args 2>&1 | ForEach-Object { "$_" }
    } finally {
        $ErrorActionPreference = $prevEap
    }
    $exit = $LASTEXITCODE
    $executeMs = Get-ExecuteMsFromOutput $out
    $stats = Get-TimeSqlStatsFromOutput $out
    return @{ execute_ms = $executeMs; stats = $stats; raw = $out; exit = $exit; precomputed = $UsePrecomputed }
}

Push-Location $tmpdir
Write-Host "Setup: $Rows rows/table VM=$Vm OPT=$Opt VERIFY=$Verify PRECOMP=$Precomputed RUNS=$TimeSqlRuns"
if ($ProfileHeavy -or $TwoMillion) {
    Write-Host "Phase D: profile via ASTRALDB_PROFILE_OUTPUT=$profileJson"
}

$phaseD = @{}

foreach ($vm in $Vm) {
  foreach ($o in $Opt) {
    foreach ($name in @("_bench_lim_only.sql", "_bench_count.sql", "_bench_join.sql", "_bench_heavy.sql",
        "_bench_join_star.sql", "_bench_heavy_semistructured.sql", "_bench_count_semistructured.sql")) {
        if ($TwoMillion -and $name -ne "_bench_heavy.sql") { continue }
        $bench = Join-Path $BenchDir $name
        if (-not (Test-Path $bench)) { continue }
        $setup = if ($name -match "semistructured") { Join-Path $BenchDir "_bench_setup_semistructured.sql" } else { $setupFile }
        if ($name -match "semistructured" -and -not (Test-Path $setup)) { continue }
        if ($name -match "semistructured") {
            $setupText = (Get-Content (Join-Path $BenchDir "_bench_setup_semistructured.sql") -Raw) -replace 'BULK\s+\d+', "BULK $Rows"
            $semSetup = Join-Path $tmpdir "setup_semistructured.sql"
            Set-Content -Path $semSetup -Value $setupText -Encoding utf8
            $setup = $semSetup
        }
        Write-Host "`n=== VM=$vm O=$o $name (precomputed ON) ==="
        $res = Invoke-TimedSql -VmName $vm -OptLevel $o -SetupPath $setup -BenchPath $bench -UsePrecomputed $true
        if ($null -ne $res.execute_ms) {
            if ($TimeSqlRuns -gt 1) {
                Write-Host ("median_execute_ms={0} (runs={1}, min={2}, max={3})" -f $res.execute_ms, $TimeSqlRuns, $res.stats.execute_min_ms, $res.stats.execute_max_ms)
            } else {
                Write-Host ("execute_ms={0}" -f $res.execute_ms)
            }
            $res.raw | Select-String "\[time-sql\]" | Select-Object -Last 1
            $res.raw | Select-String "\[time-sql-profile\]" | ForEach-Object { Write-Host $_.Line }
            if ($TwoMillion) {
                $phaseD.precomputed_on_ms = $res.execute_ms
                $phaseD.result_rows = $res.stats.result_rows
                $phaseD.fast_path_flags = $res.stats.fast_path_flags
            }
        } else {
            Write-Host "ERROR: no [time-sql] line (exit=$($res.exit))"
            $res.raw | Select-Object -Last 8 | ForEach-Object { Write-Host $_ }
        }

        if ($TwoMillion -and $name -eq "_bench_heavy.sql") {
            Write-Host "`n=== VM=$vm O=$o $name (precomputed OFF parity) ==="
            $off = Invoke-TimedSql -VmName $vm -OptLevel $o -SetupPath $setup -BenchPath $bench -UsePrecomputed $false
            if ($null -ne $off.execute_ms) {
                Write-Host ("median_execute_ms={0}" -f $off.execute_ms)
                $off.raw | Select-String "\[time-sql\]" | Select-Object -Last 1
                $phaseD.precomputed_off_ms = $off.execute_ms
                if ($phaseD.result_rows -ne $off.stats.result_rows) {
                    Write-Host "WARN: result_rows mismatch ON=$($phaseD.result_rows) OFF=$($off.stats.result_rows)"
                } else {
                    Write-Host "parity: result_rows=$($phaseD.result_rows) OK"
                }
            }
        }
    }
  }
}

if ($TwoMillion -and $phaseD.Count -gt 0) {
    if (Test-Path $profileJson) {
        $phaseD.profile_json = $profileJson
        try {
            $prof = Get-Content $profileJson -Raw | ConvertFrom-Json
            if ($prof -and $prof[0].region_timings) {
                Write-Host "`nRegion timings (ns): $($prof[0].region_timings | ConvertTo-Json -Compress)"
                $phaseD.region_timings = $prof[0].region_timings
            }
        } catch {
            Write-Host "Note: could not parse profile JSON"
        }
    }
    $phaseD.rows = $Rows
    $phaseD.runs = $TimeSqlRuns
    $phaseD.timestamp = (Get-Date).ToString("o")
    $phaseD | ConvertTo-Json -Depth 5 | Set-Content -Path $summaryJson -Encoding utf8
    Write-Host "`nPhase D summary: $summaryJson"
}

Pop-Location
