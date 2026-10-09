#!/usr/bin/env python3
"""Create a small vertical EPUB for checking Issue #43 punctuation placement."""

from html import escape
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZIP_STORED, ZipFile


DEST = Path(__file__).resolve().parent / "epubs" / "yomuka_issue43_comma_period.epub"
STYLE = "body{writing-mode:vertical-rl;-epub-writing-mode:vertical-rl;} p{margin:0 0.8em;}"


def chapter(title, paragraphs):
    body = "".join(f"<p>{paragraph}</p>" for paragraph in paragraphs)
    return (f'<?xml version="1.0" encoding="utf-8"?>'
            f'<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="ja">'
            f'<head><title>{escape(title)}</title><style>{STYLE}</style></head>'
            f'<body><h1>{escape(title)}</h1>{body}</body></html>').encode("utf-8")


base = "あいうえおかきくけこさしすせそたちつてとなにぬねのはひふへほまみむめも"
punctuation = [
    "この章は、句読点が縦書きの列末に来たときの確認用です。文字サイズを変えると、列末に来る位置も変わります。",
]
for mark in ("、", "。"):
    for length in range(12, 39, 2):
        lead = (base * 2)[:length]
        punctuation.append(f"{lead}{mark}次の文字はここから続きます。{lead}{mark}次の文字はここから続きます。")

ordinary = [
    "今日は小さな本を開いた。庭の木には新しい葉が出て、風が静かに吹いていた。",
    "棚から一冊を選び、数ページ読んだ。短い文と長い文を交互に置いて、列の終わりを確かめる。",
] * 12

controls = [
    "「開き括弧」と「閉じ括弧」の位置を確認します。小さいかなぁぃぅぇぉゃゅょも確認します。",
    "<ruby>漢字<rt>かんじ</rt></ruby>のルビがある列では、従来どおりの改列を確認します。",
    "読点、句点。括弧「文章」読点、句点。括弧「文章」読点、句点。",
] * 6

container = b'''<?xml version="1.0" encoding="utf-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
<rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles>
</container>'''
package = '''<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="bookid" xml:lang="ja">
<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
<dc:identifier id="bookid">urn:uuid:bc90cf22-12e4-4e20-957b-18968def0043</dc:identifier>
<dc:title>縦書き句読点の確認（Issue 43）</dc:title><dc:language>ja</dc:language>
<dc:creator>Yomuka test fixture</dc:creator><meta property="dcterms:modified">2026-10-09T00:00:00Z</meta>
</metadata><manifest>
<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
<item id="a" href="punctuation.xhtml" media-type="application/xhtml+xml"/>
<item id="b" href="ordinary.xhtml" media-type="application/xhtml+xml"/>
<item id="c" href="controls.xhtml" media-type="application/xhtml+xml"/>
</manifest><spine><itemref idref="a"/><itemref idref="b"/><itemref idref="c"/></spine></package>'''
nav = '''<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" xml:lang="ja">
<head><title>目次</title></head><body><nav epub:type="toc"><ol>
<li><a href="punctuation.xhtml">句読点</a></li><li><a href="ordinary.xhtml">通常の文章</a></li>
<li><a href="controls.xhtml">括弧とルビ</a></li>
</ol></nav></body></html>'''

with ZipFile(DEST, "w") as epub:
    epub.writestr("mimetype", "application/epub+zip", compress_type=ZIP_STORED)
    epub.writestr("META-INF/container.xml", container, compress_type=ZIP_DEFLATED)
    epub.writestr("OEBPS/content.opf", package.encode("utf-8"), compress_type=ZIP_DEFLATED)
    epub.writestr("OEBPS/nav.xhtml", nav.encode("utf-8"), compress_type=ZIP_DEFLATED)
    epub.writestr("OEBPS/punctuation.xhtml", chapter("句読点", punctuation), compress_type=ZIP_DEFLATED)
    epub.writestr("OEBPS/ordinary.xhtml", chapter("通常の文章", ordinary), compress_type=ZIP_DEFLATED)
    epub.writestr("OEBPS/controls.xhtml", chapter("括弧とルビ", controls), compress_type=ZIP_DEFLATED)

print(DEST)
