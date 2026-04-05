#!/usr/bin/env python3
"""
Write a custom OTA URL into the device's NVS over USB.
This avoids reflashing — the device will use this URL on next boot.

Usage:
    python set_ota_url.py                          # use default URL
    python set_ota_url.py <url>                    # use custom URL
    python set_ota_url.py --port COM5 <url>        # specify port

The device must be power-cycled into download mode (hold BOOT + power cycle).
"""
import sys, os, struct, hashlib, tempfile, subprocess

PORT   = 'COM3'
BAUD   = 460800
NVS_OFFSET = 0x9000
NVS_SIZE   = 0x4000

DEFAULT_OTA_URL = 'https://www.danscodellaro.com/esp32/xiaozhi/ota.php'

# Parse args
args = sys.argv[1:]
url = DEFAULT_OTA_URL
for i, a in enumerate(args):
    if a == '--port' and i+1 < len(args):
        PORT = args[i+1]
    elif not a.startswith('--'):
        url = a

print(f'Target OTA URL: {url}')
print(f'Port: {PORT}')
print()

# We use esptool to write directly to the NVS partition.
# The safest approach: read current NVS, patch the ota_url key, write back.
# For simplicity we use nvs_partition_gen to create a fresh NVS with just ota_url set.

# Generate a minimal NVS CSV with the ota_url entry
csv_content = f"""key,type,encoding,value
wifi,namespace,,
ota_url,data,string,{url}
"""

with tempfile.TemporaryDirectory() as tmpdir:
    csv_path = os.path.join(tmpdir, 'nvs_patch.csv')
    bin_path = os.path.join(tmpdir, 'nvs_patch.bin')

    with open(csv_path, 'w') as f:
        f.write(csv_content)

    # Generate NVS binary using nvs_partition_gen
    nvs_gen = r'C:\esp\v5.5.3\esp-idf\components\nvs_flash\nvs_partition_generator\nvs_partition_gen.py'
    python  = r'C:\Espressif\tools\python\v5.5.3\venv\Scripts\python.exe'

    env = dict(os.environ)
    for k in ['MSYSTEM','MSYSTEM_PREFIX','MSYSTEM_CHOST','MINGW_PREFIX','MSYSTEM_CARCH','OSTYPE','SHELL']:
        env.pop(k, None)

    r = subprocess.run(
        [python, nvs_gen, 'generate', csv_path, bin_path, str(NVS_SIZE)],
        env=env, capture_output=True, text=True
    )
    if r.returncode != 0:
        print('ERROR generating NVS binary:')
        print(r.stderr)
        sys.exit(1)
    print('NVS binary generated.')

    # Flash the NVS partition
    print(f'Flashing NVS to device at offset 0x{NVS_OFFSET:x}...')
    print('Make sure device is in download mode (hold BOOT + power cycle)!')
    input('Press Enter when ready...')

    r = subprocess.run([
        python, '-m', 'esptool',
        '--chip', 'esp32s3',
        '--port', PORT,
        '--baud', str(BAUD),
        '--before', 'no-reset',
        'write_flash',
        f'0x{NVS_OFFSET:x}', bin_path
    ], env=env)

    if r.returncode == 0:
        print()
        print(f'Done! OTA URL set to: {url}')
        print('Power cycle the device normally — it will use the new OTA URL on next boot.')
    else:
        print('Flash failed.')
        sys.exit(1)
