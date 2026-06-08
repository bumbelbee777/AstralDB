param(
    [Parameter(Mandatory = $true)]
    [string]$InputPath,
    [string]$Upx = $env:ASTRALDB_UPX_BIN
)

if (-not $Upx) { $Upx = "upx.exe" }
if (-not (Test-Path $Upx)) {
    $cmd = Get-Command $Upx -ErrorAction SilentlyContinue
    if ($cmd) { $Upx = $cmd.Source }
}
if (-not (Test-Path $Upx)) {
    $RepoUpx = Join-Path (Split-Path $PSScriptRoot -Parent) "upx.exe"
    if (Test-Path $RepoUpx) { $Upx = $RepoUpx }
}
if (-not (Test-Path $Upx)) {
    Write-Error "UPX not found: $Upx"
    exit 1
}

$Backup = "$InputPath.unpacked"
if (-not (Test-Path $Backup)) {
    Copy-Item -Force $InputPath $Backup
}
Copy-Item -Force $Backup $InputPath
& $Upx --best --lzma --strip-relocs=0 --force-overwrite -q -o $InputPath $Backup
& $Upx -t $InputPath
$raw = (Get-Item $Backup).Length
$packed = (Get-Item $InputPath).Length
Write-Output "Packed $(Split-Path $InputPath -Leaf): $raw -> $packed bytes"
