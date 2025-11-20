# Compile Vulkan Shaders to SPIR-V
# Run this after modifying any .vert, .frag, or .comp files

$VULKAN_SDK = $env:VULKAN_SDK
if (-not $VULKAN_SDK) {
    Write-Error "VULKAN_SDK environment variable not set. Install Vulkan SDK."
    exit 1
}

$glslc = "$VULKAN_SDK\Bin\glslc.exe"
if (-not (Test-Path $glslc)) {
    Write-Error "glslc.exe not found at $glslc"
    exit 1
}

$shaderDir = "shaders\vulkan"
$outputDir = "build\bin\shaders\vulkan"

# Create output directory
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null

Write-Host "=== Compiling Vulkan Shaders ===" -ForegroundColor Cyan

# Compile all shaders
Get-ChildItem "$shaderDir\*.vert", "$shaderDir\*.frag", "$shaderDir\*.comp" | ForEach-Object {
    $inputFile = $_.FullName
    $outputFile = Join-Path $outputDir ($_.Name + ".spv")
    
    Write-Host "Compiling $($_.Name)..." -ForegroundColor Yellow
    & $glslc -o $outputFile $inputFile
    
    if ($LASTEXITCODE -eq 0) {
        Write-Host "  ✓ $($_.Name) -> $($_.Name).spv" -ForegroundColor Green
    } else {
        Write-Host "  ✗ Failed to compile $($_.Name)" -ForegroundColor Red
        exit 1
    }
}

Write-Host "`n=== Shader Compilation Complete ===" -ForegroundColor Cyan
Write-Host "Output: $outputDir" -ForegroundColor Gray
