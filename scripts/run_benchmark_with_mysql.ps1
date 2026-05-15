# Starts MySQL in Docker, waits until healthy, runs scripts/benchmark_torture_plot.py with matching
# credentials, then stops the container (unless -KeepContainer).
#
# Requires: Docker Desktop (or Docker Engine) with compose v2 (`docker compose`).
#
# Usage (from repo root):
#   powershell -ExecutionPolicy Bypass -File scripts/run_benchmark_with_mysql.ps1
#   powershell -ExecutionPolicy Bypass -File scripts/run_benchmark_with_mysql.ps1 -RestartDockerDesktop
#
# If `docker info` returns HTTP 500 for every API version, the Linux engine is wedged — use
# -RestartDockerDesktop, or quit Docker Desktop fully, run `wsl --shutdown`, start Docker again.
# Optional: only if you see failures specifically with v1.24 in the URL, try:
#   $env:DOCKER_API_VERSION='1.43'; powershell ... -File scripts/run_benchmark_with_mysql.ps1

param(
	[int]$Runs = 3,
	[double]$Scale = 0.001,
	[string]$Output = "benchmark_torture_plot.png",
	[switch]$NoFetch,
	[switch]$KeepContainer,
	[switch]$SkipDuckdb,
	[switch]$SkipSqlite,
	[switch]$RestartDockerDesktop,
	[int]$EngineWaitSec = 180
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Compose = Join-Path $PSScriptRoot "docker/mysql-benchmark-compose.yml"
$Project = "astraldb-bench-mysql"

function Invoke-Docker {
	param([Parameter(Mandatory)][string[]]$DockerArgs)
	$err = Join-Path ([System.IO.Path]::GetTempPath()) ("astraldb-docker-err-" + [Guid]::NewGuid().ToString() + ".txt")
	$out = Join-Path ([System.IO.Path]::GetTempPath()) ("astraldb-docker-out-" + [Guid]::NewGuid().ToString() + ".txt")
	try {
		$p = Start-Process -FilePath "docker" -ArgumentList $DockerArgs -Wait -PassThru -NoNewWindow `
			-RedirectStandardError $err -RedirectStandardOutput $out
		return $p.ExitCode
	} finally {
		Remove-Item -LiteralPath $err, $out -ErrorAction SilentlyContinue
	}
}

function Write-DockerTroubleshoot {
	Write-Host ""
	Write-Host "Docker engine is not responding (often HTTP 500 on /v1.xx/info). Try in order:" -ForegroundColor Yellow
	Write-Host "  1) Docker Desktop tray icon -> Quit Docker Desktop, wait 10s, start it again." -ForegroundColor Yellow
	Write-Host "  2) In an elevated PowerShell:  wsl --shutdown   then start Docker Desktop." -ForegroundColor Yellow
	Write-Host "  3) Re-run this script with:  -RestartDockerDesktop" -ForegroundColor Yellow
	Write-Host "  4) Docker Desktop -> Troubleshoot -> Restart or Reset to factory defaults (last resort)." -ForegroundColor Yellow
	Write-Host ""
}

function Restart-DockerDesktopProcess {
	$dd = "C:\Program Files\Docker\Docker\Docker Desktop.exe"
	Write-Host "Restarting Docker Desktop (closes UI; other containers stop) ..." -ForegroundColor Cyan
	Get-Process -Name "Docker Desktop" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
	Start-Sleep -Seconds 8
	if (Test-Path -LiteralPath $dd) {
		Start-Process -FilePath $dd
	} else {
		Write-Error "Docker Desktop.exe not found at $dd"
	}
	Start-Sleep -Seconds 15
}

function Test-DockerCompose {
	if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
		Write-Error "Docker is not on PATH. Install Docker Desktop / Docker Engine, then retry."
	}
	$ec = Invoke-Docker -DockerArgs @("compose", "version")
	if ($ec -ne 0) {
		Write-Error "'docker compose' failed (exit $ec). Install Docker Compose v2 plugin."
	}
}

function Wait-DockerEngine {
	param([int]$TimeoutSec, [int]$IntervalSec = 3)
	$deadline = (Get-Date).AddSeconds($TimeoutSec)
	while ((Get-Date) -lt $deadline) {
		$ec = Invoke-Docker -DockerArgs @("info")
		if ($ec -eq 0) {
			return
		}
		Write-Host "Waiting for Docker engine (docker info exit $ec) ..." -ForegroundColor Yellow
		Start-Sleep -Seconds $IntervalSec
	}
	Write-DockerTroubleshoot
	Write-Error "Docker engine did not become ready within ${TimeoutSec}s."
}

if ($RestartDockerDesktop) {
	Restart-DockerDesktopProcess
}

Test-DockerCompose
Wait-DockerEngine -TimeoutSec $EngineWaitSec

Write-Host "Starting MySQL ($Project) from $Compose ..." -ForegroundColor Cyan
Push-Location $Root
$benchExit = 1
try {
	$ecUp = Invoke-Docker -DockerArgs @(
		"compose", "-f", $Compose, "-p", $Project, "up", "-d", "--wait"
	)
	if ($ecUp -ne 0) {
		Write-DockerTroubleshoot
		throw "docker compose up failed (exit $ecUp)."
	}

	$mysqlArgs = @(
		"--repo-root", $Root,
		"--runs", "$Runs",
		"--scale", "$Scale",
		"--output", $Output,
		"--mysql-host", "127.0.0.1",
		"--mysql-port", "13306",
		"--mysql-user", "root",
		"--mysql-password", "astraldb_bench_root",
		"--mysql-database", "astraldb_bench"
	)
	if ($NoFetch) { $mysqlArgs += "--no-fetch" }
	if ($SkipDuckdb) { $mysqlArgs += "--skip-duckdb" }
	if ($SkipSqlite) { $mysqlArgs += "--skip-sqlite" }

	Write-Host "Running benchmark_torture_plot.py ..." -ForegroundColor Cyan
	& python (Join-Path $PSScriptRoot "benchmark_torture_plot.py") @mysqlArgs
	$benchExit = $LASTEXITCODE
}
finally {
	if (-not $KeepContainer) {
		Write-Host "Stopping MySQL container ..." -ForegroundColor Cyan
		$null = Invoke-Docker -DockerArgs @("compose", "-f", $Compose, "-p", $Project, "down", "-v")
	} else {
		Write-Host "Leaving container running (--KeepContainer). Tear down later with:" -ForegroundColor Yellow
		Write-Host "  docker compose -f `"$Compose`" -p $Project down -v" -ForegroundColor Yellow
	}
	Pop-Location
}

if ($benchExit -ne 0) {
	exit $benchExit
}
