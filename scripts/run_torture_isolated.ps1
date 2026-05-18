# Runs torture_test.sql in a disposable working directory and subprocess context so heavy I/O
# does not use the repository root (avoids locking or slowing the IDE on large runs).
param(
	[string]$SqlPath = "",
	[string]$AstralDbExe = ""
)
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
if(-not $SqlPath) {
	$SqlPath = Join-Path $Root "examples\torture_test.sql"
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
		Write-Error "Build AstralDB first (looked for astraldb.exe under bin\, build\Release\, build-cmake\Release\, or build-ci\ under $Root)."
	}
}
$SqlPath = (Resolve-Path -LiteralPath $SqlPath).Path
$Tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("astraldb_torture_" + [guid]::NewGuid().ToString("n"))
New-Item -ItemType Directory -Path $Tmp | Out-Null
try {
	Push-Location $Tmp
	& $AstralDbExe -O2 --time-sql $SqlPath
	$code = $LASTEXITCODE
} finally {
	Pop-Location
	Remove-Item -LiteralPath $Tmp -Recurse -Force -ErrorAction SilentlyContinue
}
exit $code
