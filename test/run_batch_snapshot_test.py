"""Compile actual batch export + XML probe with real Expat and fake SD/ZIP boundary."""
import argparse
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--compiler', default='g++')
parser.add_argument('--zig', action='store_true')
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='yomuka-batch-') as directory:
    path = Path(directory)
    objects = []
    for name in ('xmlparse','xmlrole','xmltok'):
        obj = path / (name + '.o')
        cc = [args.compiler,'cc'] if args.zig else ['gcc']
        subprocess.run(cc + ['-O2','-DXML_STATIC','-DXML_GE=0','-DXML_CONTEXT_BYTES=1024','-I'+str(root/'lib/expat'),'-c',str(root/'lib/expat'/f'{name}.c'),'-o',str(obj)],check=True,timeout=120)
        objects.append(str(obj))
    command = [args.compiler] + (['c++'] if args.zig else [])
    command += ['-std=c++20','-Wall','-Wextra','-Werror','-O2','-DXML_STATIC',
                '-I'+str(root/'test/batch_snapshot/stubs'),'-I'+str(root/'test/sync_storage/stubs'),
                '-I'+str(root/'src'),'-I'+str(root/'lib/expat'),'-isystem',str(root/'.pio/libdeps/default/ArduinoJson/src')]
    if args.zig:
        compat=path/'time.h'
        compat.write_text('#include <ctime>\ninline std::tm* localtime_r(const std::time_t* t,std::tm* out){auto p=std::localtime(t);if(!p)return nullptr;*out=*p;return out;}\n')
        command += ['-include',str(compat)]
    sources=['StorageIo','SyncProgress','ProgressStorage','BookmarkStorage','SnapshotValidation','ImportTransaction','SnapshotExchange','BatchSnapshot','SnapshotBookProbe']
    command += [str(root/'src/sync'/f'{name}.cpp') for name in sources]
    command += [str(root/name) for name in ['src/BookReaderSettings.cpp','src/ReadingHistoryStore.cpp','test/batch_snapshot/test_batch_snapshot.cpp']]
    exe=path/'test.exe'
    subprocess.run(command+objects+['-o',str(exe)],check=True,timeout=180)
    subprocess.run([str(exe)],check=True,timeout=60)
