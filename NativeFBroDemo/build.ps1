$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildDir = Join-Path $root "build"

cmake -S $root -B $buildDir -A x64
cmake --build $buildDir --config Debug
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE"
}

Write-Host ""
Write-Host "Built: $buildDir\Debug\NativeFBroDemo.exe"
