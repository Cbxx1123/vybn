# Links the current build to output/main-dev.exe instead of output/main.exe, for testing while the
# app is open (it holds main.exe). Run after `cmake --build build` has compiled everything.
Set-Location (Join-Path $PSScriptRoot "..\build")
$env:PATH = "C:\TDM-GCC-64\bin;$env:PATH"
$lines = Get-Content CMakeFiles\main.dir\link.txt | ForEach-Object {
    $_.Replace('output\main.exe', 'output\main-dev.exe').Replace('output/main.exe', 'output/main-dev.exe')
}
foreach ($l in $lines) {
    cmd /c $l
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
