"""Compile real discovery/catalog/decoders against an in-memory SD and ZIP identity boundary."""
import argparse
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--compiler', default='g++')
parser.add_argument('--zig', action='store_true')
args = parser.parse_args()
sources = ['src/sync/ShareCandidateCatalog.cpp', 'src/sync/ShareCandidateScanner.cpp',
           'src/sync/ShareSearchExclusions.cpp',
           'src/sync/StorageIo.cpp', 'src/sync/SyncProgress.cpp', 'src/sync/BookmarkStorage.cpp', 'src/sync/ProgressStorage.cpp',
           'src/BookReaderSettings.cpp', 'src/ReadingHistoryStore.cpp', 'src/util/BookDataPath.cpp',
           'test/share_candidates/test_share_candidates.cpp']
with tempfile.TemporaryDirectory(prefix='yomuka-share-candidates-') as directory:
    path = Path(directory)
    command = [args.compiler] + (['c++'] if args.zig else [])
    command += ['-std=c++20', '-Wall', '-Wextra', '-Werror', '-O2',
                '-I' + str(root / 'test/share_candidates/stubs'),
                '-I' + str(root / 'test/sync_storage/stubs'), '-I' + str(root / 'src'),
                '-isystem', str(root / '.pio/libdeps/default/ArduinoJson/src')]
    if args.zig:
        # Windows has localtime_s rather than POSIX localtime_r (single-threaded test).
        compat = path / 'localtime.h'
        compat.write_text('#include <ctime>\ninline std::tm* localtime_r(const std::time_t* t, std::tm* out) { '
                          'auto value = std::localtime(t); if (!value) return nullptr; *out = *value; return out; }\n')
        command += ['-include', str(compat)]
    exe = path / 'test.exe'
    command += [str(root / item) for item in sources] + ['-o', str(exe)]
    subprocess.run(command, check=True, timeout=180)
    subprocess.run([str(exe)], check=True, timeout=60)
