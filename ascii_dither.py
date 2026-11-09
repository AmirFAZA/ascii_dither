#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
================================================================================
 ascii_dither.py — JPEG → JPEG из ASCII-символов 16 стандартных ANSI-цветов
                   с контурной обводкой объектов и логами прогресса.
================================================================================

ЗАПУСК:
    python ascii_dither.py <input> <output> [опции]

КЛЮЧЕВЫЕ ФЛАГИ:
    -f, --font-size N       размер шрифта, px (по умолчанию ~min(W,H)/40)
    -j, --jitter F          разброс позиции внутри ячейки 0..1 (0.40)
    -e, --edge-threshold F  порог детекции края 0..1 (0.12)
    --no-edges              отключить контуры
    --scale K               апскейл выходного холста (1.0)
    --bg {black,white}      цвет подложки (black)
    -s, --seed N            зерно случайных позиций (42)
    -q, --quiet             подавить логи
    -h, --help              справка

Логи идут в stderr с таймстампами и прогресс-барами; --quiet отключает их.
Результат сохраняется как JPEG (quality=95, subsampling=0).
================================================================================
"""

import argparse
import math
import random
import sys
import time
from PIL import Image, ImageDraw, ImageFont


# ------------------------------------------------------------------
# Логирование
# ------------------------------------------------------------------
_T0 = time.time()
_QUIET = False


def log(msg):
    if _QUIET:
        return
    sys.stderr.write(f"[{time.time() - _T0:6.2f}s] {msg}\n")
    sys.stderr.flush()


def progress(label, i, total, step_pct=5):
    """Прогресс-бар в stderr, обновляется каждые step_pct процентов."""
    if _QUIET:
        return
    if i > total:
        i = total
    pct = int(i * 100 / max(1, total))
    if pct < progress.last.get(label, -step_pct) + step_pct and i < total:
        return
    progress.last[label] = pct
    n = 20
    filled = int(n * pct / 100)
    bar = "█" * filled + "░" * (n - filled)
    sys.stderr.write(f"\r  {label}: [{bar}] {pct:3d}%")
    sys.stderr.flush()
    if i >= total:   # <-- Теперь >=, чтобы наверняка поставить \n
        sys.stderr.write("\n")
progress.last = {}


# ------------------------------------------------------------------
# 16 стандартных ANSI-цветов
# ------------------------------------------------------------------
PALETTE = [
    (0,   0,   0), (128, 0,   0), (0,   128, 0), (128, 128, 0),
    (0,   0,   128), (128, 0,   128), (0,   128, 128), (192, 192, 192),
    (128, 128, 128), (255, 0,   0), (0,   255, 0), (255, 255, 0),
    (0,   0,   255), (255, 0,   255), (0,   255, 255), (255, 255, 255),
]

GLYPH_COV = {
    ' ': 0.00, '.': 0.04, ':': 0.08, '-': 0.12, '=': 0.18,
    '+': 0.20, '*': 0.22, '#': 0.30, '%': 0.38, 'M': 0.40, '@': 0.50,
}


def color_dist2(c1, c2):
    return (0.299*(c1[0]-c2[0])**2 +
            0.587*(c1[1]-c2[1])**2 +
            0.114*(c1[2]-c2[2])**2)


def find_font(size):
    candidates = [
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
        "C:/Windows/Fonts/consola.ttf",
        "C:/Windows/Fonts/cour.ttf",
        "C:/Windows/Fonts/lucon.ttf",
        "/System/Library/Fonts/Menlo.ttc",
        "/System/Library/Fonts/Monaco.ttf",
        "DejaVuSansMono.ttf",
        "arial.ttf",
    ]
    for p in candidates:
        try:
            return ImageFont.truetype(p, size)
        except (OSError, IOError):
            continue
    log("  шрифт: используется load_default() (моноширинного не найдено)")
    return ImageFont.load_default()


def build_options(bg_rgb):
    opts = []
    for pi, p in enumerate(PALETTE):
        for g, c in GLYPH_COV.items():
            if c < 0.02:
                continue
            for k in (1, 2):
                eff = min(0.90, c * k * 0.85)
                perc = tuple(eff*p[i] + (1 - eff)*bg_rgb[i] for i in range(3))
                opts.append((pi, g, k, perc))
    return opts


def dither(small, bg_rgb, options):
    w, h = small.size
    px = small.load()
    result = [[None]*w for _ in range(h)]
    er = [[[0.0, 0.0, 0.0] for _ in range(w)] for _ in range(h)]

    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            e = er[y][x]
            tr = max(0.0, min(255.0, r + e[0]))
            tg = max(0.0, min(255.0, g + e[1]))
            tb = max(0.0, min(255.0, b + e[2]))
            tc = (tr, tg, tb)

            tl = (0.299*tr + 0.587*tg + 0.114*tb) / 255.0
            max_k = 2 if tl > 0.35 else 1

            best, best_d = None, float('inf')
            for opt in options:
                if opt[2] > max_k:
                    continue
                d = color_dist2(tc, opt[3])
                if d < best_d:
                    best_d, best = d, opt
            result[y][x] = best

            dr = tr - best[3][0]
            dg = tg - best[3][1]
            db = tb - best[3][2]
            for dx, dy, kk in ((1, 0, 7), (-1, 1, 3), (0, 1, 5), (1, 1, 1)):
                nx, ny = x + dx, y + dy
                if 0 <= nx < w and 0 <= ny < h:
                    f = kk / 16.0
                    er[ny][nx][0] += dr * f
                    er[ny][nx][1] += dg * f
                    er[ny][nx][2] += db * f
        progress("дизеринг", y + 1, h, step_pct=10)
    return result


def detect_edges(small):
    w, h = small.size
    px = small.load()
    gray = [[0.0]*w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            gray[y][x] = (299*r + 587*g + 114*b) / 1000.0

    mag = [[0.0]*w for _ in range(h)]
    ang = [[0.0]*w for _ in range(h)]

    # Теперь range(h), а не range(1, h-1)
    for y in range(h):
        # Считаем Собеля только для внутренних пикселей
        if 1 <= y < h - 1:
            for x in range(1, w - 1):
                gx = (gray[y-1][x+1] + 2*gray[y][x+1] + gray[y+1][x+1]
                      - gray[y-1][x-1] - 2*gray[y][x-1] - gray[y+1][x-1])
                gy = (gray[y+1][x-1] + 2*gray[y+1][x] + gray[y+1][x+1]
                      - gray[y-1][x-1] - 2*gray[y-1][x] - gray[y-1][x+1])
                mag[y][x] = math.hypot(gx, gy)
                ang[y][x] = math.degrees(math.atan2(gy, gx)) % 180.0
        
        # Прогресс теперь честно дойдёт до h (100%)
        progress("края Собеля", y + 1, h, step_pct=20)

    return mag, ang


def edge_glyph(angle_deg, strength):
    a = angle_deg
    if a < 22.5 or a >= 157.5:
        return '|'
    if a < 67.5:
        return '/'
    if a < 112.5:
        return '__' if strength > 1.6 else '--'
    return '\\'


# ------------------------------------------------------------------
def render(inp, out, font_size, jitter, scale, bg_choice, seed,
           use_edges=True, edge_threshold=0.12):

    random.seed(seed)
    log(f"входной файл: {inp}")
    src = Image.open(inp).convert("RGB")
    W, H = src.size
    log(f"  размер исходника: {W}×{H}")

    W2 = max(1, int(W * scale))
    H2 = max(1, int(H * scale))

    cell_h = font_size
    cell_w = max(3, int(round(font_size * 0.6)))
    cw = max(1, W2 // cell_w)
    ch = max(1, H2 // cell_h)
    log(f"  холст: {W2}×{H2},  ячеек: {cw}×{ch}  "
        f"({cell_w}×{cell_h} px/ячейка, шрифт {font_size}px, scale {scale})")

    if W2 * H2 > 25_000_000:
        log(f"  ⚠ холст {W2}×{H2} — большой, рендер может занять несколько минут")

    small = src.resize((cw, ch), Image.LANCZOS)
    log("  изображение уменьшено до сетки ячеек")

    bg_rgb = (0, 0, 0) if bg_choice == "black" else (255, 255, 255)
    options = build_options(bg_rgb)
    log(f"  цветов в палитре: {len(PALETTE)}, вариантов краски: {len(options)}")
    log("  дизеринг Флойда–Штейнберга...")
    choices = dither(small, bg_rgb, options)

    if use_edges:
        log("  детекция краёв (Sobel)...")
        mag, ang = detect_edges(small)
        max_mag = max((max(row) for row in mag), default=0.0) or 1.0
        norm_mag = [[m / max_mag for m in row] for row in mag]
        n_edges = sum(1 for row in norm_mag for m in row if m > edge_threshold)
        log(f"  ячеек-контуров: {n_edges} "
            f"({100*n_edges/max(1, cw*ch):.1f}%, порог {edge_threshold})")
    else:
        log("  контуры отключены (--no-edges)")
        norm_mag, ang = None, None

    log(f"  рендер символов (jitter={jitter})...")
    canvas = Image.new("RGB", (W2, H2), bg_rgb)
    draw = ImageDraw.Draw(canvas)
    font = find_font(font_size)

    for cy in range(ch):
        for cx in range(cw):
            pi, fill_glyph, k, _ = choices[cy][cx]
            color = PALETTE[pi]
            bx = cx * cell_w
            by = cy * cell_h

            if use_edges and norm_mag[cy][cx] > edge_threshold:
                glyph = edge_glyph(ang[cy][cx], norm_mag[cy][cx])
                dx = int(random.uniform(-0.10, 0.10) * cell_w)
                dy = int(random.uniform(-0.10, 0.10) * cell_h)
                if glyph.startswith('_'):
                    dy -= cell_h // 3
                xy = (bx + dx, by + dy)
                try:
                    draw.text(xy, glyph, font=font, fill=color, anchor="lt")
                except (TypeError, ValueError):
                    draw.text(xy, glyph, font=font, fill=color)
                continue

            if fill_glyph == ' ':
                continue
            for _ in range(k):
                dx = int(random.uniform(-jitter, jitter) * cell_w)
                dy = int(random.uniform(-jitter, jitter) * cell_h)
                xy = (bx + dx, by + dy)
                try:
                    draw.text(xy, fill_glyph, font=font, fill=color, anchor="lt")
                except (TypeError, ValueError):
                    draw.text(xy, fill_glyph, font=font, fill=color)
        progress("рендер", cy + 1, ch, step_pct=5)

    log(f"  сохранение в {out} (JPEG quality=95, subsampling=0)...")
    canvas.save(out, "JPEG", quality=95, subsampling=0)
    log(f"готово, {out}")


def main():
    global _QUIET

    ap = argparse.ArgumentParser(
        description="JPEG → JPEG из ASCII-символов 16 цветов, "
                    "с контурами и логами прогресса.")
    ap.add_argument("input",  help="входной JPEG/PNG")
    ap.add_argument("output", help="выходной .jpg")
    ap.add_argument("-f", "--font-size", type=int, default=None,
                    help="размер шрифта в пикселях")
    ap.add_argument("-j", "--jitter", type=float, default=0.40,
                    help="разброс позиции символа в ячейке 0..1 (0.40)")
    ap.add_argument("-e", "--edge-threshold", type=float, default=0.12,
                    help="порог обнаружения края 0..1 (0.12)")
    ap.add_argument("--no-edges", action="store_true",
                    help="отключить контурную обводку")
    ap.add_argument("--scale", type=float, default=1.0,
                    help="масштаб выходного изображения (1.0)")
    ap.add_argument("--bg", choices=["black", "white"], default="black",
                    help="цвет подложки (black)")
    ap.add_argument("-s", "--seed", type=int, default=42,
                    help="зерно генератора случайных позиций (42)")
    ap.add_argument("-q", "--quiet", action="store_true",
                    help="подавить логи")
    args = ap.parse_args()

    _QUIET = args.quiet

    src = Image.open(args.input)
    W, H = src.size
    fs = args.font_size or max(8, min(W, H) // 40)

    log("=" * 60)
    log(f"ascii_dither: {args.input} → {args.output}")
    log(f"  шрифт={fs}px, jitter={args.jitter}, "
        f"края={'off' if args.no_edges else args.edge_threshold}, "
        f"scale={args.scale}, bg={args.bg}, seed={args.seed}")
    log("=" * 60)

    try:
        render(args.input, args.output, fs, args.jitter,
               args.scale, args.bg, args.seed,
               use_edges=not args.no_edges,
               edge_threshold=args.edge_threshold)
    except KeyboardInterrupt:
        log("прервано пользователем (Ctrl+C)")
        sys.exit(130)


if __name__ == "__main__":
    main()
