#!/usr/bin/env python3
"""Generate the committed vertical circled-number verification EPUB."""

from pathlib import Path
from zipfile import ZIP_DEFLATED, ZIP_STORED, ZipFile


ROOT = Path(__file__).resolve().parent.parent
OUTPUT = ROOT / "test" / "epubs" / "yomuka_vertical_circled_numbers_check.epub"

FILES = {
    "META-INF/container.xml": """<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>
""",
    "OEBPS/content.opf": """<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid" version="3.0" xml:lang="ja"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="bookid">urn:uuid:5d7b46d3-75f0-4517-8093-93722f75ced4</dc:identifier><dc:title>縦書き丸数字確認</dc:title><dc:language>ja</dc:language></metadata><manifest><item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="chapter"/></spine></package>
""",
    "OEBPS/nav.xhtml": """<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" xml:lang="ja"><head><title>目次</title></head><body><nav epub:type="toc"><ol><li><a href="chapter.xhtml">確認ページ</a></li></ol></nav></body></html>
""",
    "OEBPS/chapter.xhtml": """<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="ja"><head><title>縦書き丸数字確認</title><style>body{writing-mode:vertical-rl;line-height:1.7;margin:0.8em;}h1{font-size:1.2em;}p{margin:0 0.7em;}</style></head><body>
<h1>縦書き 丸数字確認</h1>
<p>連続：①②③④⑤⑥⑦⑧⑨⑩⑪⑫⑬⑭⑮⑯⑰⑱⑲⑳</p>
<p>前後比較：あ①い　あ⑩い　あ⑳い</p>
<p>句読点との比較：①、②。③「④」</p>
<p>確認項目：丸数字が正立し、前後の文字と同じ一文字分の送りになること。</p>
</body></html>
""",
}


def main() -> None:
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    with ZipFile(OUTPUT, "w") as archive:
        archive.writestr("mimetype", "application/epub+zip", compress_type=ZIP_STORED)
        for name, contents in FILES.items():
            archive.writestr(name, contents.encode("utf-8"), compress_type=ZIP_DEFLATED)
    print(OUTPUT)


if __name__ == "__main__":
    main()
