
Write-Host "Fetching Dear ImGui..."
if (!(Get-Command git -ErrorAction SilentlyContinue)) {
    Write-Error "git not found in PATH"
    exit 1
}
if (!(Test-Path "L4D2VRConfigTool\external")) {
    New-Item -ItemType Directory -Path "L4D2VRConfigTool\external" | Out-Null
}
Set-Location "L4D2VRConfigTool\external"
git clone https://github.com/ocornut/imgui.git
Write-Host "Done. Reopen solution and build."
Pause
