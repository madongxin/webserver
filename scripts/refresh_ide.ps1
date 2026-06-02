# Windows：在本机重新生成 compile_commands.json（路径必须是 Windows 路径）
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot\..

Write-Host ">>> cmake configure (build/)"
cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host ">>> sync compile_commands.json to project root"
cmake --build build --target ide_compile_commands
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if (Test-Path "compile_commands.json") {
    Write-Host "OK: compile_commands.json ready"
    Write-Host "Next: Cursor -> Ctrl+Shift+P -> Developer: Reload Window"
} else {
    Write-Host "ERROR: compile_commands.json not found"
    exit 1
}
