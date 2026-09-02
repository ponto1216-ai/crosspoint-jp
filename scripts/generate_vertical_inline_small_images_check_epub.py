#!/usr/bin/env python3
"""Generate a synthetic EPUB for vertical inline small-image regression checks."""

from pathlib import Path
from struct import pack
from zlib import compress, crc32
from zipfile import ZIP_DEFLATED, ZIP_STORED, ZipFile


ROOT = Path(__file__).resolve().parent.parent
OUTPUT = ROOT / "test" / "epubs" / "yomuka_vertical_inline_small_images_check.epub"


def chunk(kind: bytes, data: bytes) -> bytes:
    return pack(">I", len(data)) + kind + data + pack(">I", crc32(kind + data) & 0xFFFFFFFF)


def make_png(width: int, height: int, pattern: int) -> bytes:
    """Create a compact, copyright-free grayscale test illustration."""
    stroke = max(3, min(width, height) // 16)
    rows = []
    for y in range(height):
        row = bytearray()
        for x in range(width):
            border = x < stroke or x >= width - stroke or y < stroke or y >= height - stroke
            if pattern == 0:
                ink = border or abs(x - y) < stroke or abs(x + y - (width - 1)) < stroke
            elif pattern == 1:
                ink = border or abs(y - height // 2) < stroke
            else:
                ink = border or abs(x - width // 2) < stroke
            row.append(0 if ink else 255)
        rows.append(b"\x00" + bytes(row))
    raw = b"".join(rows)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0)) +
            chunk(b"IDAT", compress(raw, 9)) + chunk(b"IEND", b""))


FILES = {
    "META-INF/container.xml": """<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>
""",
    "OEBPS/content.opf": """<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid" version="3.0" xml:lang="ja"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="bookid">urn:uuid:abec402c-ca99-4dba-8a62-b35ac384862a</dc:identifier><dc:title>縦書き本文小画像確認</dc:title><dc:language>ja</dc:language></metadata><manifest><item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/><item id="square" href="image/square.png" media-type="image/png"/><item id="wide" href="image/wide.png" media-type="image/png"/><item id="tall" href="image/tall.png" media-type="image/png"/></manifest><spine><itemref idref="chapter"/></spine></package>
""",
    "OEBPS/nav.xhtml": """<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" xml:lang="ja"><head><title>目次</title></head><body><nav epub:type="toc"><ol><li><a href="chapter.xhtml">確認ページ</a></li></ol></nav></body></html>
""",
    "OEBPS/chapter.xhtml": """<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="ja"><head><title>縦書き本文小画像確認</title><style>body{writing-mode:vertical-rl;line-height:1.7;margin:0.8em;}h1{font-size:1.2em;}p{margin:0 0.7em;}</style></head><body>
<h1>縦書き 本文小画像確認</h1>
<p>正方形：前<img style="width:1em;height:1em;vertical-align:middle;" src="image/square.png" alt="図"/>後</p>
<p>横長：前<img style="width:1em;height:1em;vertical-align:middle;" src="image/wide.png" alt="図"/>後</p>
<p>縦長：前<img style="width:1em;height:1em;vertical-align:middle;" src="image/tall.png" alt="図"/>後</p>
<p>連続：あ<img style="width:1em;height:1em;vertical-align:middle;" src="image/square.png" alt="図"/>い<img style="width:1em;height:1em;vertical-align:middle;" src="image/wide.png" alt="図"/>う</p>
<p>確認項目：図版が本文列の中に収まり、前後の文字と重ならないこと。</p>
</body></html>
""",
}


def main() -> None:
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    with ZipFile(OUTPUT, "w") as archive:
        archive.writestr("mimetype", "application/epub+zip", compress_type=ZIP_STORED)
        for name, contents in FILES.items():
            archive.writestr(name, contents.encode("utf-8"), compress_type=ZIP_DEFLATED)
        # Small source images keep PNG decoding well within X3's contiguous
        # heap budget. The stroke calculation above preserves visible marks
        # after CSS reduces them to a single text cell.
        archive.writestr("OEBPS/image/square.png", make_png(64, 64, 0), compress_type=ZIP_DEFLATED)
        archive.writestr("OEBPS/image/wide.png", make_png(64, 32, 1), compress_type=ZIP_DEFLATED)
        archive.writestr("OEBPS/image/tall.png", make_png(32, 64, 2), compress_type=ZIP_DEFLATED)
    print(OUTPUT)


if __name__ == "__main__":
    main()
