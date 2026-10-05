"""Real protocol/publication helpers against the in-memory SD boundary."""
import argparse
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser()
parser.add_argument('--compiler',default='g++')
parser.add_argument('--zig',action='store_true')
args=parser.parse_args()
with tempfile.TemporaryDirectory(prefix='yomuka-direct-') as directory:
    path=Path(directory)
    command=[args.compiler]+(['c++'] if args.zig else [])
    command+=['-std=c++20','-Wall','-Wextra','-Werror','-O2','-I'+str(root/'test/sync_storage/stubs'),'-I'+str(root/'src'),'-isystem',str(root/'.pio/libdeps/default/ArduinoJson/src')]
    if args.zig:
        compat=path/'time.h'
        compat.write_text('#include <ctime>\ninline std::tm* localtime_r(const std::time_t* t,std::tm* out){auto p=std::localtime(t);if(!p)return nullptr;*out=*p;return out;}\n')
        command+=['-include',str(compat)]
    activity=(root/'src/activities/network/DirectShareActivity.cpp').read_text()
    methods=''.join(activity[activity.index('void DirectShareActivity::'+name+'('):activity.index('void DirectShareActivity::'+next_name+'(')] for name,next_name in [('fail','prepare'),('readReply','respond'),('respond','receive'),('receive','loop')])
    preparation=activity[activity.index('void DirectShareActivity::prepare()'):activity.index('  if (transfer.done())',activity.index('void DirectShareActivity::prepare()'))]
    methods+=preparation.replace('::prepare()', '::prepareOffline()')+'\n}\n'
    import re
    methods=re.sub(r'tr\((STR_[A-Z_]+)\)',r'"\1"',methods)
    fixture=(root/'test/direct_share/transport_fixture.h').read_text()
    test_source=(root/'test/direct_share/test_direct_share.cpp').read_text()
    test_source=test_source.replace('  std::cout <<', '  CHECK(transportTests(payload)==0);\n  std::cout <<')
    test_source=test_source.replace('int main() {',fixture+'\n'+methods+'\nint main() {')
    generated=path/'test.cpp'
    generated.write_text(test_source)
    sources=['StorageIo','SyncProgress','ProgressStorage','BookmarkStorage','SnapshotValidation','ImportTransaction','SnapshotExchange','DirectShareProtocol']
    command+=[str(root/'src/sync'/f'{name}.cpp') for name in sources]
    command+=[str(root/name) for name in ['src/BookReaderSettings.cpp','src/ReadingHistoryStore.cpp']]
    command+=[str(generated)]
    exe=path/'test.exe'
    subprocess.run(command+['-o',str(exe)],check=True,timeout=180)
    subprocess.run([str(exe)],check=True,timeout=60)
