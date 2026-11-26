import argparse
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--compiler", required=True)
parser.add_argument("--zig", action="store_true")
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
output = root / "build/txt_to_html"
output.mkdir(parents=True, exist_ok=True)
exe = output / "TxtToHtmlTest.exe"
command = [args.compiler] + (["c++"] if args.zig else [])
command += ["-std=c++20", "-Wall", "-Wextra", "-Werror"]
command += ["-I" + str(root / path) for path in ["test/txt_to_html/stubs", "lib/Txt"]]
command += [str(root / path) for path in [
    "test/txt_to_html/TxtToHtmlTest.cpp", "lib/Txt/Txt.cpp", "lib/Txt/TxtToHtml.cpp"]]
subprocess.run(command + ["-o", str(exe)], check=True)
subprocess.run([str(exe)], check=True)
