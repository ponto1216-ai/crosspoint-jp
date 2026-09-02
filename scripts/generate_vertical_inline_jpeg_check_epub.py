#!/usr/bin/env python3
"""Generate a low-memory JPEG inline-image verification EPUB for X3."""

from io import BytesIO
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZIP_STORED, ZipFile

from PIL import Image, ImageDraw


ROOT = Path(__file__).resolve().parent.parent
OUTPUT = ROOT / "test" / "epubs" / "yomuka_vertical_inline_jpeg_check.epub"


def make_marker_jpeg() -> bytes:
    """Create a copyright-free 64px framed cross without external assets."""
    image = Image.new("L", (64, 64), 255)
    draw = ImageDraw.Draw(image)
    draw.rectangle((5, 5, 58, 58), outline=0, width=6)
    draw.line((12, 12, 51, 51), fill=0, width=6)
    draw.line((51, 12, 12, 51), fill=0, width=6)
    output = BytesIO()
    image.save(output, format="JPEG", quality=90, optimize=True)
    return output.getvalue()


FILES = {
    "META-INF/container.xml": """<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>
""",
    "OEBPS/content.opf": """<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid" version="3.0" xml:lang="ja"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="bookid">urn:uuid:11a9892e-53cc-4fcd-b69a-aa0592f33e11</dc:identifier><dc:title>縦書き本文JPEG画像確認</dc:title><dc:language>ja</dc:language></metadata><manifest><item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/><item id="marker" href="image/marker.jpg" media-type="image/jpeg"/></manifest><spine><itemref idref="chapter"/></spine></package>
""",
    "OEBPS/nav.xhtml": """<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" xml:lang="ja"><head><title>目次</title></head><body><nav epub:type="toc"><ol><li><a href="chapter.xhtml">確認ページ</a></li></ol></nav></body></html>
""",
    "OEBPS/chapter.xhtml": """<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="ja"><head><title>縦書き本文JPEG画像確認</title><style>body{writing-mode:vertical-rl;line-height:1.7;margin:0.8em;}h1{font-size:1.2em;}p{margin:0 0.7em;}</style></head><body>
<h1>縦書き 本文JPEG画像確認</h1>
<p>前<img style="width:1em;height:1em;vertical-align:middle;" src="image/marker.jpg" alt="図"/>後</p>
<p>連続：あ<img style="width:1em;height:1em;vertical-align:middle;" src="image/marker.jpg" alt="図"/>い<img style="width:1em;height:1em;vertical-align:middle;" src="image/marker.jpg" alt="図"/>う</p>
<p>確認項目：小さいJPEG画像が本文列に表示され、前後の文字と重ならないこと。</p>
</body></html>
""",
}


def main() -> None:
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    with ZipFile(OUTPUT, "w") as archive:
        archive.writestr("mimetype", "application/epub+zip", compress_type=ZIP_STORED)
        for name, contents in FILES.items():
            archive.writestr(name, contents.encode("utf-8"), compress_type=ZIP_DEFLATED)
        archive.writestr("OEBPS/image/marker.jpg", make_marker_jpeg(), compress_type=ZIP_DEFLATED)
    print(OUTPUT)


if __name__ == "__main__":
    main()
