"""Exercise production TXT caching, EPUB adapter, and metadata serialization."""
import argparse
from pathlib import Path
import subprocess
parser = argparse.ArgumentParser()
parser.add_argument("--compiler", required=True)
parser.add_argument("--zig", action="store_true")
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
output = root / "build/txt_cache"
output.mkdir(parents=True, exist_ok=True)
source = (root / "lib/Epub/Epub.cpp").read_text(encoding="utf-8")
methods = source[source.index("uint8_t* Epub::readItemContentsToBytes"):
                 source.index("int Epub::getSpineItemsCount")]
(output / "TxtEpubMethods.inc").write_text(methods, encoding="utf-8")
status = (root / "src/ReadingStatusHelper.cpp").read_text(encoding="utf-8")
(output / "TxtProgressMethods.inc").write_text(
    status[status.index("ReadingProgress readProgress("):status.index("bool bookmarkFileHasEntries(")],
    encoding="utf-8")
parser_source = (root / "lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp").read_text(encoding="utf-8")
start = parser_source.index("bool ChapterHtmlSlimParser::addExplicitBlankLine()")
(output / "TxtBlankLines.inc").write_text(
    parser_source[start:parser_source.index("\n}", start) + 2], encoding="utf-8")

command = [args.compiler] + (["c++"] if args.zig else [])
command += ["-std=c++20", "-Wall", "-Wextra", "-Wno-sign-compare"]
command += ["-I" + str(root / path) for path in [
    "test/txt_to_html/stubs", "lib/Txt", "lib/Epub", "lib/Serialization", "src", "build/txt_cache"]]
for test in ["TxtCacheTest", "TxtProgressTest"]:
    exe = output / (test + ".exe")
    sources = [
        "test/txt_to_html/" + test + ".cpp", "lib/Txt/Txt.cpp", "lib/Txt/TxtToHtml.cpp",
        "lib/Txt/TxtCache.cpp", "lib/Epub/Epub/BookMetadataCache.cpp"]
    subprocess.run(command + [str(root / path) for path in sources] + ["-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
