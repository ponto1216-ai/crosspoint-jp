#!/usr/bin/env python3
"""Compile the production HAL against host I2C/clock/ADC stubs."""
import argparse
import os
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--compiler', default=os.environ.get('CXX', 'c++'))
parser.add_argument('--zig', action='store_true')
args = parser.parse_args()
out = root / 'build' / 'battery_diagnostics'
out.mkdir(parents=True, exist_ok=True)
for c3, level in [(1, 1), (1, 2), (0, 1)]:
    exe = out / f'battery_c3_{c3}_level_{level}.exe'
    command = [args.compiler] + (['c++'] if args.zig else [])
    command += ['-std=c++20', '-O0', f'-DFREEINK_MCU_C3={c3}', f'-DLOG_LEVEL={level}',
                f'-I{root / "test/battery_diagnostics/stubs"}', f'-I{root / "lib/hal"}',
                str(root / 'test/battery_diagnostics/BatteryDiagnosticsTest.cpp'),
                str(root / 'lib/hal/HalPowerManager.cpp'), '-o', str(exe)]
    subprocess.run(command, check=True)
    subprocess.run([str(exe)], check=True)

    edv_exe = out / f'edv2_c3_{c3}_level_{level}.exe'
    edv_command = [str(edv_exe) if item == str(exe) else
                   str(root / 'test/battery_diagnostics/Edv2DiagnosticsTest.cpp')
                   if item == str(root / 'test/battery_diagnostics/BatteryDiagnosticsTest.cpp') else item
                   for item in command]
    subprocess.run(edv_command, check=True)
    subprocess.run([str(edv_exe)], check=True)

exe = out / 'capacity_test.exe'
command = [args.compiler] + (['c++'] if args.zig else [])
command += ['-std=c++20', '-O0', f'-I{root / "test/battery_diagnostics/stubs"}',
            f'-I{root / "lib/hal"}', str(root / 'test/battery_diagnostics/Bq27220CapacityTest.cpp'), '-o', str(exe)]
subprocess.run(command, check=True)
subprocess.run([str(exe)], check=True)
