# Run Quasar integration tests against bin/astraldb.exe
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Bin = Join-Path $Root "bin\astraldb.exe"
if (-not (Test-Path $Bin)) {
    Write-Error "Missing bin/astraldb.exe - build AstralDB and place the executable under bin/"
}
$env:QUASAR_ASTRALDB = $Bin
Remove-Item Env:QUASAR_TEST_MOCK -ErrorAction SilentlyContinue
Write-Host "Using $Bin"
& $Bin -v
Set-Location $Root
$integrationTest = 'quasar/tests/test_quasar.py::test_migration_bundle_integration'
python -m pytest quasar/tests/test_migrate_util.py $integrationTest -q --tb=short
exit $LASTEXITCODE
