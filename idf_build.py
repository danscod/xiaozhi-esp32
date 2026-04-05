#!/usr/bin/env python3
"""
Build helper for xiaozhi-esp32 on Windows.
Strips MSYS/MinGW env vars that block idf.py, sets up proper ESP-IDF environment.

Usage:
    python idf_build.py <idf.py args>
    python idf_build.py set-target esp32s3
    python idf_build.py menuconfig
    python idf_build.py build
    python idf_build.py flash --port COM3
"""
import subprocess, os, sys

IDF_VERSION = 'v5.5.3'
IDF_PATH = rf'C:\esp\{IDF_VERSION}\esp-idf'
IDF_PYTHON = rf'C:\Espressif\tools\python\{IDF_VERSION}\venv\Scripts\python.exe'
IDF_PY = rf'C:\esp\{IDF_VERSION}\esp-idf\tools\idf.py'

# Toolchain installed by idf_tools.py ends up under tools/tools/ due to IDF_TOOLS_PATH nesting
XTENSA = r'C:\Espressif\tools\tools\xtensa-esp-elf\esp-14.2.0_20251107\xtensa-esp-elf\bin'

TOOLS_PATH = ';'.join([
    rf'C:\Espressif\tools\python\{IDF_VERSION}\venv\Scripts',
    XTENSA,
    r'C:\Espressif\tools\cmake\3.30.2\bin',
    r'C:\Espressif\tools\ninja\1.12.1',
    r'C:\Program Files\Git\mingw64\bin',
    r'C:\Windows\System32',
    r'C:\Windows',
])

env = dict(os.environ)
for k in ['MSYSTEM', 'MSYSTEM_PREFIX', 'MSYSTEM_CHOST', 'MINGW_PREFIX',
          'MSYSTEM_CARCH', 'OSTYPE', 'SHELL']:
    env.pop(k, None)

env['IDF_PATH'] = IDF_PATH
env['IDF_PYTHON_ENV_PATH'] = rf'C:\Espressif\tools\python\{IDF_VERSION}\venv'
env['IDF_TOOLS_PATH'] = r'C:\Espressif\tools'
env['PATH'] = TOOLS_PATH

args = sys.argv[1:] if len(sys.argv) > 1 else ['--version']
cmd = [IDF_PYTHON, IDF_PY] + args

print(f'Running: idf.py {" ".join(args)}', flush=True)
result = subprocess.run(cmd, env=env)
sys.exit(result.returncode)
