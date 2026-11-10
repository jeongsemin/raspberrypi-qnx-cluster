# Builds ClusterConsole.exe from cluster_console.py (run in PowerShell on Windows).
# It makes a virtual environment in %TEMP%, installs PyInstaller and pywebview there, and writes
# dist\ClusterConsole.exe next to this script. Nothing is installed into your system Python.
# The exe carries the whole project folder (payload.zip, made by make_payload.py from qnx-everywhere-projects) and the
# scripts of its environment check, so it can install the projects into WSL on a PC that has none.
# Close a running ClusterConsole.exe first: Windows keeps the file locked while it runs.
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$venv = Join-Path $env:TEMP "cluster_console_venv"
$py = Join-Path $venv "Scripts\python.exe"
# check for python.exe, not the folder: a temp cleaner can delete Scripts\ and leave the rest behind
if (-not (Test-Path $py)) { python -m venv --clear $venv; if ($LASTEXITCODE -ne 0) { throw "python -m venv failed" } }
# pip and PyInstaller print notices on stderr, which Windows PowerShell 5.1 can turn into a stopping error;
# judge them by their exit code instead
$ErrorActionPreference = "Continue"
& $py -m pip install --quiet pyinstaller pywebview 2>$null
if ($LASTEXITCODE -ne 0) { throw "pip install failed" }

$payload = Join-Path $venv "payload.zip"
& $py (Join-Path $here "make_payload.py") $payload
if ($LASTEXITCODE -ne 0) { throw "make_payload.py failed" }

$data = @("payload.zip", "env_check.sh", "install_payload.sh") | ForEach-Object {
    $src = if ($_ -eq "payload.zip") { $payload } else { Join-Path $here $_ }
    "--add-data"; "$src;."
}
& $py -m PyInstaller --noconfirm --onefile --noconsole --name ClusterConsole --icon (Join-Path $here "cluster.ico") `
    --distpath (Join-Path $here "dist") --workpath (Join-Path $venv "work") --specpath (Join-Path $venv "work") `
    @data (Join-Path $here "cluster_console.py") 2>&1 | Select-Object -Last 3
if ($LASTEXITCODE -ne 0) { throw "PyInstaller failed (is ClusterConsole.exe still running?)" }
# the notes for other people (SmartScreen, what is needed) go next to the exe: hand over both files
Copy-Item (Join-Path $here "README.txt") (Join-Path $here "dist\README.txt") -Force
Write-Host "built: $here\dist\ClusterConsole.exe (+ README.txt)"
