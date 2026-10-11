"""Compare simulator captures against an explicitly approved image baseline."""

import argparse
from html import escape
from pathlib import Path
import sys

from PIL import Image, ImageChops


def compare(baseline: Path, captures: Path, output: Path) -> int:
    if not baseline.is_dir() or not captures.is_dir():
        raise ValueError("Both baseline and capture directories must exist")
    expected = {p.name: p for p in baseline.iterdir() if p.suffix.lower() in (".bmp", ".png")}
    actual = {p.name: p for p in captures.iterdir() if p.suffix.lower() in (".bmp", ".png")}
    if not expected:
        raise ValueError("Baseline has no BMP or PNG images")
    output.mkdir(parents=True, exist_ok=True)
    failures = 0
    rows = []
    for name in sorted(expected.keys() | actual.keys()):
        if name not in expected or name not in actual:
            failures += 1
            rows.append((name, "missing capture" if name not in actual else "unexpected capture", ""))
            continue
        with Image.open(expected[name]) as source, Image.open(actual[name]) as current:
            if source.size != current.size:
                failures += 1
                rows.append((name, f"size {source.size} → {current.size}", ""))
                continue
            difference = ImageChops.difference(source.convert("RGB"), current.convert("RGB"))
            bbox = difference.getbbox()
            if bbox is None:
                rows.append((name, "identical", ""))
                continue
            failures += 1
            changed = sum(1 for pixel in difference.getdata() if pixel != (0, 0, 0))
            mask = difference.convert("L").point(lambda value: 255 if value else 0)
            highlighted = current.convert("RGB")
            highlighted.paste((255, 0, 0), mask=mask)
            diff_name = f"{Path(name).stem}-diff.png"
            highlighted.save(output / diff_name)
            rows.append((name, f"{changed} pixels; bbox {bbox}", diff_name))
    items = "\n".join(
        f"<tr><td>{escape(name)}</td><td>{escape(result)}</td><td>"
        + (f'<img src="{escape(diff)}" alt="difference">' if diff else "") + "</td></tr>"
        for name, result, diff in rows
    )
    (output / "index.html").write_text(
        '<!doctype html><html lang="ja"><meta charset="utf-8"><title>Yomuka screenshot comparison</title>'
        '<style>body{font-family:sans-serif}td{border:1px solid #aaa;padding:8px;vertical-align:top}'
        'img{max-width:500px}table{border-collapse:collapse}</style>'
        f'<h1>Yomuka screenshot comparison</h1><p>{failures} differences</p>'
        f'<table><tr><th>Image</th><th>Result</th><th>Changed pixels</th></tr>{items}</table></html>',
        encoding="utf-8",
    )
    print(f"Compared {len(expected)} baseline images: {failures} differences. Report: {output / 'index.html'}")
    return 1 if failures else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("captures", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        return compare(args.baseline, args.captures, args.output)
    except ValueError as error:
        print(error, file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
