param(
    [Parameter(Mandatory = $true)]
    [string]$BinaryPath,
    [string]$Upx = $env:ASTRALDB_UPX_BIN
)

if (-not (Test-Path $BinaryPath)) {
    Write-Error "Binary not found: $BinaryPath"
    exit 1
}

$raw = (Get-Item $BinaryPath).Length
Write-Output "raw_bytes=$raw"

if (-not $Upx) { $Upx = "upx.exe" }
if (-not (Get-Command $Upx -ErrorAction SilentlyContinue)) {
    $RepoUpx = Join-Path (Split-Path $PSScriptRoot -Parent) "upx.exe"
    if (Test-Path $RepoUpx) { $Upx = $RepoUpx }
}
if (Test-Path $Upx) {
    $tmp = [System.IO.Path]::GetTempFileName()
    try {
        Copy-Item -Force $BinaryPath "${tmp}.unpacked"
        & $Upx --best --lzma --strip-relocs=0 --force-overwrite -q -o $tmp "${tmp}.unpacked" 2>$null
        if ($LASTEXITCODE -eq 0) {
            $upx = (Get-Item $tmp).Length
            Write-Output "upx_bytes=$upx"
            Write-Output ("ratio={0:N3}" -f ($upx / $raw))
        }
    } finally {
        Remove-Item -Force $tmp -ErrorAction SilentlyContinue
        Remove-Item -Force "${tmp}.unpacked" -ErrorAction SilentlyContinue
    }
}
