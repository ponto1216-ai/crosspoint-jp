#!/usr/bin/env python3
"""Create a copyright-safe EPUB for UAX #50 vertical-orientation checks."""

from __future__ import annotations

import argparse
import tempfile
import zipfile
from pathlib import Path


MIMETYPE = b"application/epub+zip"


def write(path: Path, content: str) -> None:
    path.write_text(content, encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path, nargs="?", default=Path("test/epubs/yomuka_vertical_orientation_uax50.epub"))
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        (root / "META-INF").mkdir()
        (root / "OEBPS").mkdir()
        write(root / "META-INF" / "container.xml", """<?xml version="1.0"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles>
</container>""")
        write(root / "OEBPS" / "content.opf", """<?xml version="1.0" encoding="utf-8"?>
<package version="3.0" xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid">
 <metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="bookid">yomuka-uax50</dc:identifier><dc:title>UAX #50 縦書き判定</dc:title><dc:language>ja</dc:language></metadata>
 <manifest>
  <item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
  <item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/>
 </manifest>
 <spine page-progression-direction="rtl"><itemref idref="chapter"/></spine>
</package>""")
        write(root / "OEBPS" / "nav.xhtml", """<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE html><html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" xml:lang="ja"><head><title>目次</title></head>
<body><nav epub:type="toc" id="toc"><ol><li><a href="chapter.xhtml">UAX #50 縦書き判定</a></li></ol></nav></body></html>""")
        write(root / "OEBPS" / "chapter.xhtml", """<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE html><html xmlns="http://www.w3.org/1999/xhtml" xml:lang="ja"><head>
<title>UAX #50 縦書き判定</title><style>
body { writing-mode: vertical-rl; line-height: 1.8; }
h1 { font-size: 1.25em; } p { margin: 0 0 1em; }
</style></head><body>
<h1>UAX #50 縦書き判定</h1>
<p>正立（U / Tu）：①②③　ⅠⅡⅢ　℃　ℓ　№　★☆♪</p>
<p>回転（R）：←→↑↓　αβγ　АБВ　Å</p>
<p>変形（Tu / Tr）：、。：「」（）　ー　…　〜</p>
<p>半角は規格どおり回転：ｱｲｳｴｵ　ｰ</p>
<p>縦中横：1　12　123　3.14　12:34　!?　!!</p>
<p>混在：第①章→αとА、気温は20℃。</p>
</body></html>""")
        with zipfile.ZipFile(args.output, "w") as archive:
            archive.writestr("mimetype", MIMETYPE, compress_type=zipfile.ZIP_STORED)
            for relative in ("META-INF/container.xml", "OEBPS/content.opf", "OEBPS/nav.xhtml", "OEBPS/chapter.xhtml"):
                archive.write(root / relative, relative, compress_type=zipfile.ZIP_DEFLATED)


if __name__ == "__main__":
    main()
