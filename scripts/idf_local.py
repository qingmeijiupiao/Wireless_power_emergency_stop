"""Local Windows IDF launcher; configure child environment inside Python."""
import os
from pathlib import Path
import subprocess
import sys

tools = Path('C:/Espressif/tools')
idf = Path('C:/esp/v6.0/esp-idf')
venv = tools/'python/v6.0/venv'
env = dict(os.environ)
old_path = os.environ.get('PATH', '')
for key in list(env):
    if key.lower() == 'path':
        del env[key]
env.update(IDF_PATH=str(idf), IDF_TOOLS_PATH=str(tools),
           IDF_PYTHON_ENV_PATH=str(venv), ESP_IDF_VERSION='6.0',
           ESP_ROM_ELF_DIR=str(tools/'esp-rom-elfs/20241011'))
env['IDF_COMPONENT_CACHE_PATH'] = str(Path(__file__).resolve().parents[1]/'.component-cache')
env['PYTHONUTF8'] = '1'
env['PYTHONIOENCODING'] = 'utf-8'

# Component hashes describe Git blob bytes. Windows CRLF checkout conversion
# must not change downloaded dependencies; this applies only to child processes.
config_count = int(env.get('GIT_CONFIG_COUNT', '0'))
env[f'GIT_CONFIG_KEY_{config_count}'] = 'core.autocrlf'
env[f'GIT_CONFIG_VALUE_{config_count}'] = 'false'
env['GIT_CONFIG_COUNT'] = str(config_count + 1)
env['PATH'] = ';'.join(str(p) for p in [venv/'Scripts', tools/'cmake/4.0.3/bin',
    tools/'riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf/bin']) + ';' + old_path
sys.exit(subprocess.call([str(venv/'Scripts/python.exe'), str(idf/'tools/idf.py'), *sys.argv[1:]], env=env))
