#!/usr/bin/env python3
"""Generate a copyright-safe EPUB for vertical small-kana placement checks."""

from pathlib import Path
from zipfile import ZIP_DEFLATED, ZIP_STORED, ZipFile


ROOT = Path(__file__).resolve().parent.parent
OUTPUT = ROOT / "test" / "epubs" / "yomuka_vertical_small_kana_check.epub"

FILES = {
    "META-INF/container.xml": """<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles>
</container>
""",
    "OEBPS/content.opf": """<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid" version="3.0" xml:lang="ja">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="bookid">urn:uuid:b1f43c0e-19f8-4f1c-a385-99b5f5163ed5</dc:identifier>
    <dc:title>縦書き小書き仮名確認</dc:title><dc:language>ja</dc:language>
  </metadata>
  <manifest><item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/></manifest>
  <spine><itemref idref="chapter"/></spine>
</package>
""",
    "OEBPS/nav.xhtml": """<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="ja"><head><title>目次</title></head><body><nav epub:type="toc" xmlns:epub="http://www.idpf.org/2007/ops"><ol><li><a href="chapter.xhtml">確認ページ</a></li></ol></nav></body></html>
""",
    "OEBPS/chapter.xhtml": """<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="ja">
<head><title>縦書き小書き仮名確認</title><style>body{writing-mode:vertical-rl;line-height:1.7;margin:0.8em;}h1{font-size:1.2em;}p{margin:0 0.7em;}</style></head>
<body>
<h1>縦書き 小書き仮名確認</h1>
<p>確認基準：小書き仮名の字面が通常の仮名より右上に寄り、送り幅は同じ一文字分であること。</p>
<p>ひらがな：あぁいぃうぅえぇおぉ　かっきゃきゅきょ　わゎ　かゕけゖ</p>
<p>カタカナ：アァイィウゥエェオォ　カッキャキュキョ　ワヮ　カヵケヶ</p>
<p>連続比較：あぁあぁ　いぃいぃ　うぅうぅ　エェエェ　ッッ　ャュョ</p>
<p>語中比較：待っていた　小さなゃゅょ　ファイル　ティッシュ　ウォーク</p>
<p>行頭禁則：行末直前に小書き仮名が来ても、次の列の先頭へ出ないこと。</p>
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
