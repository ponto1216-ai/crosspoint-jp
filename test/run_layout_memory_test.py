#!/usr/bin/env python3
"""Compile production ParsedText with deterministic metrics and failing heap admission."""
import argparse
import os
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--compiler', default=os.environ.get('CXX', 'c++'))
parser.add_argument('--zig', action='store_true')
args = parser.parse_args()
out = root / 'build' / 'layout_memory'
out.mkdir(parents=True, exist_ok=True)
exe = out / ('LayoutMemoryTest.exe' if os.name == 'nt' else 'LayoutMemoryTest')
sources = ['test/layout_memory/LayoutMemoryTest.cpp', 'lib/Epub/Epub/ParsedText.cpp',
           'lib/Epub/Epub/hyphenation/Hyphenator.cpp', 'lib/Epub/Epub/hyphenation/LanguageRegistry.cpp',
           'lib/Epub/Epub/hyphenation/LiangHyphenation.cpp', 'lib/Epub/Epub/hyphenation/HyphenationCommon.cpp',
           'lib/Utf8/Utf8.cpp']
includes = ['test/layout_memory/stubs', 'test/text_emphasis/stubs', 'lib/Epub', 'lib/Utf8', 'lib/GfxRenderer',
            'lib/Logging']
cmd = [args.compiler] + (['c++', '-Wno-nullability-completeness'] if args.zig else [])
cmd += ['-std=c++20', '-O0', '-g', '-fno-exceptions']
cmd += [f'-I{root / p}' for p in includes]
cmd += [str(root / p) for p in sources] + ['-o', str(exe)]
subprocess.run(cmd, check=True)
subprocess.run([str(exe)], check=True)
