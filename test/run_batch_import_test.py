"""Run the actual sequential import scheduler with controlled per-book outcomes."""
import argparse, subprocess, tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--compiler',default='g++');p.add_argument('--zig',action='store_true');a=p.parse_args()
with tempfile.TemporaryDirectory(prefix='yomuka-batch-import-') as d:
 exe=Path(d)/'test.exe'
 command=[a.compiler]+(['c++'] if a.zig else [])+['-std=c++20','-O2','-Wall','-Wextra','-Werror','-I'+str(root/'src'),'-I'+str(root/'test/sync_storage/stubs'),'-isystem',str(root/'.pio/libdeps/default/ArduinoJson/src'),str(root/'src/sync/BatchImport.cpp'),str(root/'test/batch_import/test_batch_import.cpp'),'-o',str(exe)]
 subprocess.run(command,check=True,timeout=180);subprocess.run([str(exe)],check=True,timeout=30)
 loop=(root/'src/activities/home/BatchImportActivity.cpp').read_text(encoding='utf-8')
 loop=loop[loop.index('void BatchImportActivity::loop()'):loop.index('void BatchImportActivity::render(')]
 fixture=(root/'test/batch_import/ui_fixture.cpp').read_text(encoding='utf-8')
 source=Path(d)/'ui.cpp';source.write_text(fixture.replace('// ACTUAL_LOOP',loop),encoding='utf-8')
 command=[str(source) if x==str(root/'test/batch_import/test_batch_import.cpp') else x for x in command]
 subprocess.run(command,check=True,timeout=180);subprocess.run([str(exe)],check=True,timeout=30)
