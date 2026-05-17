# Runs off-CI stress scripts under examples/benchmarks/ in isolated temp dirs.
param(
	[string]$AstralDbExe = "",
	[switch]$All,
	[string]$Suite = "bulk"
)
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$BenchDir = Join-Path $Root "examples\benchmarks"
$Map = @{
	bulk = "stress_bulk_load.sql"
	wal = "stress_wal_churn.sql"
	analytics = "stress_analytics_mix.sql"
	torture = "..\torture_test.sql"
	traffic = "..\stress_traffic.sql"
	unified = "benchmark_torture_unified.sql"
}
if(-not $AstralDbExe) {
	$candidates = @(
		(Join-Path $Root "bin\astraldb.exe"),
		(Join-Path $Root "bin\astraldb_cli.exe"),
		(Join-Path $Root "build-cmake\Release\astraldb_cli.exe"),
		(Join-Path $Root "build-cmake\Release\astraldb.exe")
	)
	foreach($p in $candidates) {
		if(Test-Path -LiteralPath $p) { $AstralDbExe = $p; break }
	}
	if(-not $AstralDbExe) {
		Write-Error "Build AstralDB first."
	}
}
$Runner = Join-Path $PSScriptRoot "run_torture_isolated.ps1"
$Names = if($All) { @($Map.Keys) } else { @($Suite) }
foreach($Name in $Names) {
	if(-not $Map.ContainsKey($Name)) {
		Write-Error "Unknown suite '$Name'. Choose: $($Map.Keys -join ', ')"
	}
	$Rel = $Map[$Name]
	$SqlPath = if($Rel.StartsWith("..")) { Join-Path $Root "examples" ($Rel.Substring(3)) } else { Join-Path $BenchDir $Rel }
	if(-not (Test-Path -LiteralPath $SqlPath)) {
		Write-Error "Missing $SqlPath"
	}
	Write-Host "== stress suite: $Name =="
	$sw = [System.Diagnostics.Stopwatch]::StartNew()
	& $Runner -SqlPath $SqlPath -AstralDbExe $AstralDbExe
	if($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
	$sw.Stop()
	Write-Host ("completed in {0:N1}s" -f $sw.Elapsed.TotalSeconds)
}
exit 0
