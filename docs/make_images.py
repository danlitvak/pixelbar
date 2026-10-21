"""Renders the README images from `pixelbar --demo` (made-up data, nobody's real messages).

    python docs/make_images.py      # after .\\build.ps1; needs Pillow

column.png and settings.png are single frames, launch.gif is the first seconds after
launch recorded frame by frame with --record, and the detail-*.png close-ups are crops.
Everything is scaled up by whole pixels so the pixel font stays sharp.
"""
import csv
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent
EXE = HERE.parent / "bin" / "pixelbar.exe"


def run(*args):
    subprocess.run([str(EXE), "--demo", *args], check=True)


def gray(img):
    """Premultiplied white-on-black frames are gray already; snap to 32 levels for size."""
    return img.convert("L").point(lambda v: min(255, (v + 4) // 8 * 8))


def scaled(img, k):
    return img.resize((img.width * k, img.height * k), Image.NEAREST)


def main():
    if not EXE.exists():
        sys.exit(f"build first: {EXE} is missing")
    tmp = Path(tempfile.mkdtemp(prefix="pixelbar-"))

    run("--dump", str(tmp / "column.bmp"))
    column = gray(Image.open(tmp / "column.bmp"))
    scaled(column, 2).save(HERE / "column.png", optimize=True)

    run("--dump", str(tmp / "settings.bmp"), "--settings")
    scaled(gray(Image.open(tmp / "settings.bmp")), 2).save(HERE / "settings.png", optimize=True)

    # Close-ups: rows of the demo column at 4x, shown at 2x (each image is twice its display
    # size, so it stays sharp on high-DPI screens and averages cleanly on others).
    for name, top, bottom in (("detail-top", 0, 179), ("detail-agents", 181, 418)):
        scaled(column.crop((0, top, column.width, min(bottom, column.height))), 4).save(
            HERE / f"{name}.png", optimize=True)

    # Launch: every recorded frame, cropped to the rows the column ever used.
    rec = tmp / "rec"
    rec.mkdir()
    run("--record", str(rec))
    rows = [(int(t), int(top), int(h)) for t, top, h, _ in csv.reader(open(rec / "frames.csv"))]
    rows = [r for r in rows if r[0] <= 2200]
    bottom = max(top + h for _, top, h in rows)
    palette = [v for i in range(256) for v in (i, i, i)]
    frames, durations = [], []
    for i, (t, _, _) in enumerate(rows):
        f = gray(Image.open(rec / f"f{t:05d}.bmp")).crop((0, 0, 300, bottom))
        p = Image.frombytes("P", f.size, f.tobytes())
        p.putpalette(palette)
        frames.append(scaled(p, 2))
        nxt = rows[i + 1][0] if i + 1 < len(rows) else t + 2600  # hold the settled column
        durations.append(max(20, round((nxt - t) / 10) * 10))
    black = Image.new("P", frames[0].size, 0)
    black.putpalette(palette)
    black.save(HERE / "launch.gif", save_all=True, append_images=frames, duration=[400] + durations, loop=0,
               disposal=1)
    for f in sorted(HERE.glob("*.png")) + [HERE / "launch.gif"]:
        print(f"{f.name}: {f.stat().st_size // 1024} KB")


if __name__ == "__main__":
    main()
