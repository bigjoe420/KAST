"""Run idf.py from Git Bash on Windows, where ESP-IDF v6.1 refuses MSYSTEM.

Wraps idf.py in a clean PowerShell subprocess: MSYSTEM is stripped,
PROCESSOR_ARCHITECTURE is forced (Git Bash does not export it, and idf.py's
platform detection breaks without it), and the eim-managed ESP-IDF profile
is dot-sourced. Nothing here is machine- or checkout-specific.

Usage (from the project root, in Git Bash):

    python idf_run.py build
    python idf_run.py -p COM3 flash
    python idf_run.py -p COM3 flash monitor
"""
import subprocess
import sys
from pathlib import Path

PROFILE = r"C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1"
PROJECT = Path(__file__).resolve().parent

args = " ".join(sys.argv[1:]) if len(sys.argv) > 1 else "build"

command = (
    "Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue; "
    "$env:PROCESSOR_ARCHITECTURE='AMD64'; "
    f". '{PROFILE}' > $null; "
    f"Set-Location '{PROJECT}'; "
    f"idf.py {args}"
)

result = subprocess.run(
    ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass",
     "-Command", command],
    cwd=PROJECT,
)
print(f"\n=== idf.py {args} exit code: {result.returncode} ===")
sys.exit(result.returncode)
