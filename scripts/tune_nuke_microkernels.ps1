param(
	[string]$AstralDbExe = "",
	[int]$MaxMemoryMb = 3072,
	[int]$TimeoutSec = 300,
	[int]$Iterations = 3,
	[int]$Warmup = 1,
	[string]$KcValues = "192,256,320,384,448",
	[string]$MrValues = "2,4",
	[string]$OutFile = ""
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Runner = Join-Path $PSScriptRoot "run_nuke_isolated.ps1"
if(-not (Test-Path -LiteralPath $Runner)) {
	Write-Error "Missing runner script: $Runner"
}

if(-not $OutFile) {
	$OutFile = Join-Path $Root "build\nuke_tuning_results.csv"
}

function Parse-IntList([string]$Raw) {
	$Out = @()
	foreach($Part in ($Raw -split ",")) {
		$Trim = $Part.Trim()
		if(-not $Trim) { continue }
		$Val = 0
		if(-not [int]::TryParse($Trim, [ref]$Val)) {
			throw "Invalid integer in list: '$Trim'"
		}
		$Out += $Val
	}
	return $Out
}

function Invoke-NukeTimed([string]$Exe, [int]$MemMb, [int]$Timeout) {
	$Sw = [System.Diagnostics.Stopwatch]::StartNew()
	& $Runner -AstralDbExe $Exe -MaxMemoryMb $MemMb -TimeoutSec $Timeout
	$Exit = $LASTEXITCODE
	$Sw.Stop()
	return [pscustomobject]@{
		Seconds = $Sw.Elapsed.TotalSeconds
		ExitCode = $Exit
	}
}

function Set-KernelEnv([int]$Mr, [int]$Kc) {
	$env:ASTRALDB_SIMD_GEMV_MR = "$Mr"
	$env:ASTRALDB_SIMD_GEMV_KC = "$Kc"
}

function Clear-KernelEnv() {
	Remove-Item Env:ASTRALDB_SIMD_GEMV_MR -ErrorAction SilentlyContinue
	Remove-Item Env:ASTRALDB_SIMD_GEMV_KC -ErrorAction SilentlyContinue
}

$MrList = Parse-IntList $MrValues
$KcList = Parse-IntList $KcValues
if($Iterations -lt 1) { $Iterations = 1 }
if($Warmup -lt 0) { $Warmup = 0 }

$Rows = New-Object System.Collections.Generic.List[object]

Write-Host "[tune] baseline warmup/runs (default kernel config)"
Clear-KernelEnv
for($I = 0; $I -lt $Warmup; ++$I) {
	$W = Invoke-NukeTimed -Exe $AstralDbExe -MemMb $MaxMemoryMb -Timeout $TimeoutSec
	if($W.ExitCode -ne 0) { throw "Baseline warmup failed with exit code $($W.ExitCode)" }
}
$BaseTimes = @()
for($I = 0; $I -lt $Iterations; ++$I) {
	$R = Invoke-NukeTimed -Exe $AstralDbExe -MemMb $MaxMemoryMb -Timeout $TimeoutSec
	if($R.ExitCode -ne 0) { throw "Baseline run failed with exit code $($R.ExitCode)" }
	$BaseTimes += $R.Seconds
}
$BaseAvg = ($BaseTimes | Measure-Object -Average).Average
$Rows.Add([pscustomobject]@{
	Profile = "baseline"
	MR = ""
	KC = ""
	Iter = $Iterations
	AvgSeconds = [math]::Round($BaseAvg, 4)
	MinSeconds = [math]::Round(($BaseTimes | Measure-Object -Minimum).Minimum, 4)
	MaxSeconds = [math]::Round(($BaseTimes | Measure-Object -Maximum).Maximum, 4)
	SpeedupVsBaseline = 1.0
})

foreach($Mr in $MrList) {
	foreach($Kc in $KcList) {
		Write-Host "[tune] MR=$Mr KC=$Kc"
		Set-KernelEnv -Mr $Mr -Kc $Kc
		for($I = 0; $I -lt $Warmup; ++$I) {
			$W = Invoke-NukeTimed -Exe $AstralDbExe -MemMb $MaxMemoryMb -Timeout $TimeoutSec
			if($W.ExitCode -ne 0) { throw "Warmup failed for MR=$Mr KC=$Kc with exit code $($W.ExitCode)" }
		}
		$Times = @()
		for($I = 0; $I -lt $Iterations; ++$I) {
			$R = Invoke-NukeTimed -Exe $AstralDbExe -MemMb $MaxMemoryMb -Timeout $TimeoutSec
			if($R.ExitCode -ne 0) { throw "Run failed for MR=$Mr KC=$Kc with exit code $($R.ExitCode)" }
			$Times += $R.Seconds
		}
		$Avg = ($Times | Measure-Object -Average).Average
		$Rows.Add([pscustomobject]@{
			Profile = "kernel"
			MR = $Mr
			KC = $Kc
			Iter = $Iterations
			AvgSeconds = [math]::Round($Avg, 4)
			MinSeconds = [math]::Round(($Times | Measure-Object -Minimum).Minimum, 4)
			MaxSeconds = [math]::Round(($Times | Measure-Object -Maximum).Maximum, 4)
			SpeedupVsBaseline = [math]::Round(($BaseAvg / $Avg), 4)
		})
	}
}

Clear-KernelEnv
$Rows | Sort-Object AvgSeconds | Tee-Object -Variable Sorted | Format-Table -AutoSize

$OutDir = Split-Path -Parent $OutFile
if($OutDir -and -not (Test-Path -LiteralPath $OutDir)) {
	New-Item -ItemType Directory -Path $OutDir | Out-Null
}
$Sorted | Export-Csv -Path $OutFile -NoTypeInformation
Write-Host "[tune] wrote results to $OutFile"
