"""Compile production application units with deterministic platform fakes; no ESP-IDF/network needed."""
from pathlib import Path
import shutil, subprocess, tempfile
here=Path(__file__).resolve().parent
root=here.parents[1]
shared=root.parent/'wireless-power-components'
if not shared.exists():
    shared=root # Use downloaded managed components when the sibling checkout is unavailable.
includes=[here,here/'fakes',root/'components/app/app_types/include']
for component in ('battery_voltage','emergency_remote','emergency_ui','power_manager','runtime_settings'):
    includes += [root/'components/app'/component/'include',root/'components/app'/component/'src']
includes += [root/'components/app/emergency_ui/private_include',root/'components/overrides/espnow_link/include',root/'components/overrides/espnow_link/private_include',root/'components/overrides/espnow_link/src']
for kind,name in [('product','espnow_service_remote'),('product','espnow_service_proto'),('middleware','battery_level')]:
    p=shared/'components'/kind/name/'include'
    includes.append(p if p.exists() else root/'managed_components'/name/'include')
ui=root/'components/app/emergency_ui/src'
suites={
 'remote':[here/'remote.cpp'],
 'battery':[here/'battery.cpp'],
 'settings':[here/'settings.cpp',root/'components/app/runtime_settings/src/runtime_settings.cpp'],
 'ui':[here/'ui.cpp',ui/'emergency_ui.cpp',ui/'core/ui_manager.cpp',ui/'pages/status_page.cpp',ui/'pages/menu_page.cpp',ui/'pages/message_page.cpp'],
 'transport':[here/'transport.cpp',root/'components/overrides/espnow_link/src/espnow_link_api.cpp'],
 'export':[here/'export.cpp'],
 'diagnostics':[here/'diagnostics.cpp'],
 'power':[here/'power.cpp'],
}
compiler=shutil.which('g++')
if not compiler: raise SystemExit('g++ required')
with tempfile.TemporaryDirectory(prefix='estop-regression-') as build:
 for name,sources in suites.items():
    extra=[]
    if name=='export': extra=[root/'components/overrides/blackbox/include',root/'components/overrides/circular_flash_buffer/include']
    if name=='diagnostics': extra=[root/'components/app/app_diagnostics/include']
    if name=='power': extra=[here/'fakes_pm']
    exe=Path(build)/(name+'.exe')
    subprocess.run([compiler,'-std=c++17','-O1','-pthread',*['-I'+str(x) for x in [*extra,*includes]],*map(str,sources),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=15)
