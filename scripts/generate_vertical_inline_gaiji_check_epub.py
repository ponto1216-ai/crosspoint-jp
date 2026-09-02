#!/usr/bin/env python3
"""Generate a synthetic EPUB for vertical inline-image spacing checks."""

from pathlib import Path
from struct import pack
from zlib import compress, crc32
from zipfile import ZIP_DEFLATED, ZIP_STORED, ZipFile


ROOT = Path(__file__).resolve().parent.parent
OUTPUT = ROOT / "test" / "epubs" / "yomuka_vertical_inline_gaiji_check.epub"


def png_chunk(kind: bytes, data: bytes) -> bytes:
    return pack(">I", len(data)) + kind + data + pack(">I", crc32(kind + data) & 0xFFFFFFFF)


def make_marker_png() -> bytes:
    """Return a copyright-free 128px black marker on a white background."""
    width = height = 128
    rows = []
    for y in range(height):
        row = bytearray()
        for x in range(width):
            border = x < 8 or x >= width - 8 or y < 8 or y >= height - 8
            diagonal = abs(x - y) < 7 or abs(x + y - (width - 1)) < 7
            row.append(0 if border or diagonal else 255)
        rows.append(b"\x00" + bytes(row))
    raw = b"".join(rows)
    return (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0)) +
            png_chunk(b"IDAT", compress(raw, 9)) + png_chunk(b"IEND", b""))


FILES = {
    "META-INF/container.xml": """<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>
""",
    "OEBPS/content.opf": """<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid" version="3.0" xml:lang="ja"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="bookid">urn:uuid:774f398f-6efb-4d09-a29a-ecebc13edb7a</dc:identifier><dc:title>縦書き外字画像確認</dc:title><dc:language>ja</dc:language></metadata><manifest><item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/><item id="gaiji" href="image/gaiji-010.png" media-type="image/png"/></manifest><spine><itemref idref="chapter"/></spine></package>
""",
    "OEBPS/nav.xhtml": """<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" xml:lang="ja"><head><title>目次</title></head><body><nav epub:type="toc"><ol><li><a href="chapter.xhtml">確認ページ</a></li></ol></nav></body></html>
""",
    "OEBPS/chapter.xhtml": """<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="ja"><head><title>縦書き外字画像確認</title><style>body{writing-mode:vertical-rl;line-height:1.7;margin:0.8em;}h1{font-size:1.2em;}p{margin:0 0.7em;}img.gaiji{width:1em;height:1em;vertical-align:middle;}</style></head><body>
<h1>縦書き 外字画像確認</h1>
<p>前<img class="gaiji" style="width:1em;height:1em;vertical-align:middle;" src="image/gaiji-010.png" alt="外字"/>後</p>
<p>連続：あ<img class="gaiji" style="width:1em;height:1em;vertical-align:middle;" src="image/gaiji-010.png" alt="外字"/>い<img class="gaiji" style="width:1em;height:1em;vertical-align:middle;" src="image/gaiji-010.png" alt="外字"/>う</p>
<p>句読点：あ、<img class="gaiji" style="width:1em;height:1em;vertical-align:middle;" src="image/gaiji-010.png" alt="外字"/>。い</p>
<p>確認項目：文字間隔「最小」と通常値の両方で、画像と前後の文字が重ならないこと。</p>
</body></html>
""",
}


def main() -> None:
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    with ZipFile(OUTPUT, "w") as archive:
        archive.writestr("mimetype", "application/epub+zip", compress_type=ZIP_STORED)
        for name, contents in FILES.items():
            archive.writestr(name, contents.encode("utf-8"), compress_type=ZIP_DEFLATED)
        archive.writestr("OEBPS/image/gaiji-010.png", make_marker_png(), compress_type=ZIP_DEFLATED)
    print(OUTPUT)


if __name__ == "__main__":
    main()
