#!/usr/bin/env python3
"""Generate a synthetic vertical EPUB for Issue #43's follow-up report."""

from html import escape
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZIP_STORED, ZipFile, ZipInfo


DEST = Path(__file__).resolve().parent / "epubs" / "yomuka_issue43_followup.epub"
BASE = "春の庭には小さな花が咲いた。朝の風は静かで、空は明るかった。"


def chapter(title, paragraphs):
    body = "".join(f"<p>{escape(text)}</p>" for text in paragraphs)
    return (f'<?xml version="1.0" encoding="utf-8"?>'
            f'<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="ja">'
            f'<head><title>{escape(title)}</title><style>'
            'body{writing-mode:vertical-rl;-epub-writing-mode:vertical-rl}'
            'p{margin:0 0.8em}</style></head>'
            f'<body><h1>{escape(title)}</h1>{body}</body></html>').encode("utf-8")


# Move each target through different column-end positions without needing the
# reporter's copyrighted book or its exact font and layout settings.
punctuation = []
ellipsis = []
for lead in range(8, 36):
    prefix = (BASE * 3)[:lead]
    punctuation.append(f"{prefix}た。次の文が続きます。{prefix}小さなゃゅょっも続きます。")
    ellipsis.append(f"{prefix}生……城。次の文が続きます。{prefix}山‥‥川。")

container = b'''<?xml version="1.0" encoding="utf-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
<rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles>
</container>'''
package = '''<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="bookid" xml:lang="ja">
<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
<dc:identifier id="bookid">urn:uuid:ef428963-e315-495e-8443-5eb82f564909</dc:identifier>
<dc:title>禁則と省略記号の確認（Issue 43）</dc:title><dc:language>ja</dc:language>
<dc:creator>Yomuka test fixture</dc:creator><meta property="dcterms:modified">2026-10-11T00:00:00Z</meta>
</metadata><manifest>
<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
<item id="a" href="punctuation.xhtml" media-type="application/xhtml+xml"/>
<item id="b" href="ellipsis.xhtml" media-type="application/xhtml+xml"/>
</manifest><spine><itemref idref="a"/><itemref idref="b"/></spine></package>'''
nav = '''<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" xml:lang="ja">
<head><title>目次</title></head><body><nav epub:type="toc"><ol>
<li><a href="punctuation.xhtml">句点と小書き仮名</a></li>
<li><a href="ellipsis.xhtml">省略記号</a></li>
</ol></nav></body></html>'''


def write_entry(epub, name, content, compression):
    info = ZipInfo(name, date_time=(2026, 10, 11, 0, 0, 0))
    info.compress_type = compression
    epub.writestr(info, content)


with ZipFile(DEST, "w") as epub:
    write_entry(epub, "mimetype", "application/epub+zip", ZIP_STORED)
    write_entry(epub, "META-INF/container.xml", container, ZIP_DEFLATED)
    write_entry(epub, "OEBPS/content.opf", package.encode("utf-8"), ZIP_DEFLATED)
    write_entry(epub, "OEBPS/nav.xhtml", nav.encode("utf-8"), ZIP_DEFLATED)
    write_entry(epub, "OEBPS/punctuation.xhtml", chapter("句点と小書き仮名", punctuation), ZIP_DEFLATED)
    write_entry(epub, "OEBPS/ellipsis.xhtml", chapter("省略記号", ellipsis), ZIP_DEFLATED)

print(DEST)
