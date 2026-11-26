"""Check production migration against the existing atomic storage API contract."""
import argparse
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--compiler", required=True)
parser.add_argument("--zig", action="store_true")
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
output = root / "build/txt_migration"
output.mkdir(parents=True, exist_ok=True)
reader = (root / "src/activities/reader/EpubReaderActivity.cpp").read_text(encoding="utf-8")
start = reader.index("    // handles changes in reader settings and reset to approximate position")
(output / "TxtMigrationReflow.inc").write_text(
    reader[start:reader.index("    if (pendingPercentJump", start)], encoding="utf-8")
exe = output / "TxtMigrationTest.exe"
command = [args.compiler] + (["c++"] if args.zig else [])
command += ["-std=c++20", "-Wall", "-Wextra", "-Wno-sign-compare"]
command += ["-I" + str(root / path) for path in [
    "test/txt_to_html/stubs", "lib/Txt", "src", "build/txt_migration"]]
command += [str(root / path) for path in [
    "test/txt_to_html/TxtMigrationTest.cpp", "src/util/TxtProgressMigration.cpp",
    "lib/Txt/Txt.cpp", "lib/Txt/TxtToHtml.cpp"]]
subprocess.run(command + ["-o", str(exe)], check=True)
subprocess.run([str(exe)], check=True)
