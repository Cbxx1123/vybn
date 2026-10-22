# Links the current build to output/vybn-dev.exe instead of output/vybn.exe, for testing while the
# app is open (it holds vybn.exe). Run after `cmake --build build` has compiled everything.
Set-Location (Join-Path $PSScriptRoot "..\build")
$env:PATH = "C:\TDM-GCC-64\bin;$env:PATH"
$lines = Get-Content src\CMakeFiles\vybn.dir\link.txt | ForEach-Object {
    $_.Replace('output\vybn.exe', 'output\vybn-dev.exe').Replace('output/vybn.exe', 'output/vybn-dev.exe')
}
foreach ($l in $lines) {
    cmd /c $l
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
