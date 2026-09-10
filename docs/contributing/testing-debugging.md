# Testing and Debugging

CrossPoint runs on real hardware, so debugging usually combines local build checks and on-device logs.

## Local checks

Make sure `clang-format` 21+ is installed and available in `PATH` before running the formatting step.
If needed, see [Getting Started](./getting-started.md).

```sh
./bin/clang-format-fix
pio check --fail-on-defect low --fail-on-defect medium --fail-on-defect high
pio run
```

## Flash and monitor

Flash firmware:

```sh
pio run --target upload
```

Open serial monitor:

```sh
pio device monitor
```

Optional enhanced monitor:

```sh
python3 -m pip install pyserial colorama matplotlib
python3 scripts/debugging_monitor.py
```

## Useful bug report contents

- Firmware version and build environment
- Exact steps to reproduce
- Expected vs actual behavior
- Serial logs from boot through failure
- Results of checking only the affected book cache after saving diagnostics. Do not require clearing the entire `.crosspoint/` directory.

## Common troubleshooting references

- [User troubleshooting](../troubleshooting-ja.md)
- [Bug reporting](../reporting-bugs-ja.md)

Documentation-only changes need link/anchor checks and menu-name review against the implementation. Record X3/X4 verification of changed device steps separately in [the checklist](../development/device-verification-ja.md).
