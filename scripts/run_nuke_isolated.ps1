param(
	[string]$SqlPath = "",
	[string]$AstralDbExe = "",
	[int]$MaxMemoryMb = 2048,
	[int]$TimeoutSec = 180
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
if(-not $SqlPath) {
	$SqlPath = Join-Path $Root "examples\nuke.sql"
}
if(-not $AstralDbExe) {
	$candidates = @(
		(Join-Path $Root "bin\astraldb.exe"),
		(Join-Path $Root "bin\astraldb"),
		(Join-Path $Root "bin\AstralDB.exe"),
		(Join-Path $Root "build\Release\astraldb.exe"),
		(Join-Path $Root "build-cmake\Release\astraldb.exe"),
		(Join-Path $Root "build-ci\astraldb.exe")
	)
	foreach($p in $candidates) {
		if(Test-Path -LiteralPath $p) { $AstralDbExe = $p; break }
	}
	if(-not $AstralDbExe) {
		Write-Error "Build AstralDB first (unable to locate astraldb executable under $Root)."
	}
}

$SqlPath = (Resolve-Path -LiteralPath $SqlPath).Path
$MaxWorkingSetBytes = [int64]$MaxMemoryMb * 1024 * 1024
$Tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("astraldb_nuke_" + [guid]::NewGuid().ToString("n"))
New-Item -ItemType Directory -Path $Tmp | Out-Null

try {
	Push-Location $Tmp
	$proc = Start-Process -FilePath $AstralDbExe -ArgumentList @("-m", "-O4", "--time-sql", $SqlPath) -NoNewWindow -PassThru
	$sw = [System.Diagnostics.Stopwatch]::StartNew()
	$killed = $false
	$reason = ""
	while(-not $proc.HasExited) {
		Start-Sleep -Milliseconds 200
		$proc.Refresh()
		if($proc.WorkingSet64 -gt $MaxWorkingSetBytes) {
			$reason = "memory cap exceeded ($([math]::Round($proc.WorkingSet64 / 1MB, 2)) MB > $MaxMemoryMb MB)"
			$proc.Kill($true)
			$killed = $true
			break
		}
		if($sw.Elapsed.TotalSeconds -gt $TimeoutSec) {
			$reason = "timeout exceeded ($TimeoutSec s)"
			$proc.Kill($true)
			$killed = $true
			break
		}
	}
	$sw.Stop()
	if($killed) {
		Write-Host "[nuke-isolated] terminated: $reason"
		exit 124
	}
	exit $proc.ExitCode
}
finally {
	Pop-Location
	Remove-Item -LiteralPath $Tmp -Recurse -Force -ErrorAction SilentlyContinue
}
