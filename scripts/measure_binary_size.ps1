param(
    [Parameter(Mandatory = $true)]
    [string]$BinaryPath,
    [string]$Upx = $env:ASTRALDB_UPX_BIN
)

if (-not (Test-Path $BinaryPath)) {
    Write-Error "Binary not found: $BinaryPath"
    exit 1
}

$Src = $BinaryPath
$Unpacked = "$BinaryPath.unpacked"
if (Test-Path $Unpacked) { $Src = $Unpacked }

$raw = (Get-Item $Src).Length
Write-Output "raw_bytes=$raw"

if (-not $Upx) { $Upx = "upx.exe" }
if (-not (Get-Command $Upx -ErrorAction SilentlyContinue)) {
    $RepoUpx = Join-Path (Split-Path $PSScriptRoot -Parent) "upx.exe"
    if (Test-Path $RepoUpx) { $Upx = $RepoUpx }
}
if (-not (Test-Path $Upx)) {
    Write-Error "UPX not found: $Upx"
    exit 1
}

$tmp = [System.IO.Path]::GetTempFileName()
try {
    Copy-Item -Force $Src $tmp
    & $Upx --best --lzma --strip-relocs=0 --force-overwrite -q -o $tmp $Src
    if ($LASTEXITCODE -ne 0) {
        Write-Error "UPX pack failed for $Src"
        exit 1
    }
    & $Upx -t $tmp | Out-Null
    $upx = (Get-Item $tmp).Length
    Write-Output "upx_bytes=$upx"
    Write-Output ("ratio={0:N3}" -f ($upx / $raw))
} finally {
    Remove-Item -Force $tmp -ErrorAction SilentlyContinue
}
