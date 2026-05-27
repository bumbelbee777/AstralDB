param(
	[string]$AstralDbExe = "",
	[int]$MaxMemoryMb = 2048,
	[int]$TimeoutSec = 180
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Runner = Join-Path $PSScriptRoot "run_nuke_isolated.ps1"
if(-not (Test-Path -LiteralPath $Runner)) {
	Write-Error "Missing runner script: $Runner"
}

& $Runner -AstralDbExe $AstralDbExe -MaxMemoryMb $MaxMemoryMb -TimeoutSec $TimeoutSec
exit $LASTEXITCODE
