# Package Release Build for Distribution
param(
    [string]$Version = "v1.0.0"
)

Write-Host "Building Release version..." -ForegroundColor Green
cmake --build build --config Release

if ($LASTEXITCODE -ne 0) {
    Write-Host "Build failed!" -ForegroundColor Red
    exit 1
}

$releaseName = "Aetherial-$Version-Windows"
$releaseDir = "build\release-package\$releaseName"

Write-Host "Creating release package: $releaseName" -ForegroundColor Green

# Clean and create release directory
if (Test-Path "build\release-package") {
    Remove-Item -Recurse -Force "build\release-package"
}
New-Item -ItemType Directory -Path $releaseDir | Out-Null

# Copy executable
Write-Host "Copying executable..." -ForegroundColor Cyan
Copy-Item "build\bin\MMORPGEngine.exe" "$releaseDir\"

# Copy assets
Write-Host "Copying assets..." -ForegroundColor Cyan
Copy-Item -Recurse "assets" "$releaseDir\"

# Copy shaders
Write-Host "Copying shaders..." -ForegroundColor Cyan
Copy-Item -Recurse "shaders" "$releaseDir\"

# Create README for users
Write-Host "Creating README..." -ForegroundColor Cyan
@"
# Aetherial $Version

## How to Run
1. Double-click MMORPGEngine.exe
2. Use WASD to move, mouse to look around
3. Press ESC to exit

## System Requirements
- Windows 10/11
- OpenGL 4.1+ compatible GPU
- 2GB RAM minimum

## Troubleshooting
If the game doesn't start, ensure your graphics drivers are up to date.

Built on: $(Get-Date -Format "yyyy-MM-dd HH:mm")
"@ | Out-File -FilePath "$releaseDir\README.txt" -Encoding UTF8

# Create zip
Write-Host "Creating zip archive..." -ForegroundColor Cyan
$zipPath = "build\release-package\$releaseName.zip"
Compress-Archive -Path $releaseDir -DestinationPath $zipPath -Force

Write-Host "`nRelease package created successfully!" -ForegroundColor Green
Write-Host "Location: $zipPath" -ForegroundColor Yellow
Write-Host "`nNext steps:" -ForegroundColor Cyan
Write-Host "1. Go to https://github.com/BlackHawk185/game2/releases/new"
Write-Host "2. Create tag: $Version"
Write-Host "3. Upload: $zipPath"
Write-Host "4. Publish release"
