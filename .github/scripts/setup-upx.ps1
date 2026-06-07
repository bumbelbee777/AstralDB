param(
    [string]$Version = "4.2.4"
)

$ErrorActionPreference = "Stop"
$Root = Join-Path $env:RUNNER_TOOL_CACHE "upx\$Version"
New-Item -ItemType Directory -Force -Path $Root | Out-Null
$Exe = Join-Path $Root "upx.exe"

if (Test-Path $Exe) {
    Write-Host "UPX cache hit: $Exe"
} else {
    $Zip = Join-Path $env:RUNNER_TEMP "upx-$Version-win64.zip"
    $Url = "https://github.com/upx/upx/releases/download/v$Version/upx-$Version-win64.zip"
    Invoke-WebRequest -Uri $Url -OutFile $Zip
    Expand-Archive -Path $Zip -DestinationPath $Root -Force
    Remove-Item -Force $Zip
    $Nested = Get-ChildItem -Path $Root -Recurse -Filter "upx.exe" | Select-Object -First 1
    if ($null -eq $Nested) {
        throw "upx.exe not found after extracting $Url"
    }
    if ($Nested.FullName -ne $Exe) {
        Copy-Item -Force $Nested.FullName $Exe
    }
    Write-Host "Installed UPX $Version to $Exe"
}

Add-Content -Path $env:GITHUB_PATH -Value $Root
Add-Content -Path $env:GITHUB_ENV -Value "UPX_BIN=$Exe"
Add-Content -Path $env:GITHUB_ENV -Value "ASTRALDB_UPX_BIN=$Exe"
& $Exe -V 2>$null
if ($LASTEXITCODE -ne 0) {
    & $Exe --version
}
