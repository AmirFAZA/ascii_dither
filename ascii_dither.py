#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
================================================================================
 ascii_dither.py — JPEG → JPEG, собранный из ASCII-символов 16 стандартных
                   ANSI-цветов, с контурной обводкой объектов.
================================================================================

ЧТО ДЕЛАЕТ ПРОГРАММА
--------------------
  * Читает JPEG/PNG и перерисовывает его символами ASCII в 16 цветах
    (стандартная палитра xterm/ANSI).
  * Внутри однородных участков применяется дизеринг Флойда–Штейнберга:
    цвет, которого нет в палитре, воспроизводится СМЕСЬЮ символов
    нескольких палитровых цветов — так «внедрением другого цвета»
    получается похожий оттенок на крупных областях.
  * Символы ставятся в ПРОИЗВОЛЬНЫЕ позиции внутри своих ячеек
    (не по строгой сетке строк/столбцов), поэтому картинка не выглядит
    решёткой.
  * На границах объектов детектор краёв (Sobel) ставит символы по
    направлению контура: '|', '/', '\\', '--' (тонкая горизонталь) и
    '__' (жирная, для сильных границ).
  * Результат сохраняется как JPEG (quality=95, subsampling=0 — без
    хроматического размытия, чтобы 16 цветов оставались чистыми).

ЗАВИСИМОСТИ
-----------
  pip install pillow

ЗАПУСК (общий вид)
------------------
    python ascii_dither.py <input> <output> [опции]

ПРИМЕРЫ
-------
    # самый простой запуск, чёрный фон
    python ascii_dither.py photo.jpg out.jpg

    # крупные символы, апскейл ×2, светлая подложка
    python ascii_dither.py photo.jpg out.jpg -f 20 --scale 2 --bg white

    # детальная прорисовка: мелкие символы + максимум контуров
    python ascii_dither.py photo.jpg out.jpg -f 10 --scale 3 -e 0.08 -j 0.5

    # только заливка, без обводки
    python ascii_dither.py photo.jpg out.jpg --no-edges

    # фиксированное «зерно» — один и тот же результат при каждом запуске
    python ascii_dither.py photo.jpg out.jpg -s 42

ВСЕ ФЛАГИ
---------
  ПОЗИЦИОННЫЕ
    input                Путь к входному файлу (JPEG, PNG и т. п.).
    output               Путь к выходному файлу; формат определяется
                         расширением — рекомендуется .jpg.

  РАЗМЕР И ШРИФТ
    -f, --font-size N    Размер шрифта в пикселях. По умолчанию
                         max(8, min(ширина, высота) // 40), то есть
                         ~1/40 от меньшей стороны.
                           Меньше N → больше ячеек, детальнее и медленнее.
                           Больше N → крупнее символы, картинка грубее.
    --scale K            Масштаб выходного изображения (по умолчанию 1.0).
                         Холст = исходные размеры × K. Нужен, когда хочется
                         разглядеть символы: при мелком шрифте и большом
                         исходнике --scale 2–3 даёт крупный читаемый файл.

  РАЗБРОС СИМВОЛОВ (антисетка)
    -j, --jitter F       Разброс позиции символа внутри его ячейки,
                         доля от размера ячейки, 0..1 (по умолчанию 0.40).
                           j = 0    строгая сетка (для сравнения)
                           j ≈ 0.4  «живой» вид, символы плавают
                           j ≈ 0.7  сильный разброс, почти пуантилизм
                         Контурные символы всегда ставятся почти без
                         разброса (±0.10), иначе линия будет рваной.

  КОНТУРЫ ОБЪЕКТОВ
    -e, --edge-threshold F
                         Порог обнаружения края, 0..1 (по умолчанию 0.12).
                         Край рисуется там, где нормированный Sobel-модуль
                         превышает порог.
                           Меньше F (0.05–0.08) → больше контуров, ловятся
                                                даже слабые границы.
                           Больше F (0.20–0.30) → только очень сильные края,
                                                очертания крупных объектов.
    --no-edges           Полностью отключить контурную обводку. Полезно,
                         если хочется чистой заливки без «стенок» вокруг
                         каждой детали.

  ЦВЕТ ПОДЛОЖКИ
    --bg {black,white}   Цвет фона (по умолчанию black). Влияет и на
                         подложку, и на предсказанные цвета ячеек при
                         дизеринге, поэтому для светлых исходников
                         правильнее --bg white.

  ВОСПРОИЗВОДИМОСТЬ
    -s, --seed N         Зерно генератора случайных позиций (по умолчанию
                         42). С одним и тем же N и теми же флагами
                         результат байт-в-байт воспроизводим. Любое другое
                         N даёт другой «рисунок» тех же цветов.

  ПРОЧЕЕ
    -h, --help           Показать справку по всем флагам и выйти.

ПАЛИТРА (16 ЦВЕТОВ)
-------------------
    0  black    (0,0,0)          8  grey      (128,128,128)
    1  maroon   (128,0,0)        9  red       (255,0,0)
    2  green    (0,128,0)       10  lime      (0,255,0)
    3  olive    (128,128,0)     11  yellow    (255,255,0)
    4  navy     (0,0,128)       12  blue      (0,0,255)
    5  purple   (128,0,128)     13  fuchsia   (255,0,255)
    6  teal     (0,128,128)     14  aqua      (0,255,255)
    7  silver   (192,192,192)   15  white     (255,255,255)

КАК ЭТО РАБОТАЕТ (кратко)
-------------------------
  1. Исходник уменьшается до сетки «ячеек» (LANCZOS) — по одной
     точке на ячейку.
  2. Floyd–Steinberg выбирает для каждой ячейки палитровый цвет,
     символ-заполнитель и число наложений так, чтобы визуальный средний
     цвет совпал с целью.
  3. Собель находит края и их направления; на краях символ подменяется
     на штрих, соответствующий ориентации контура.
  4. Все символы рисуются в случайных позициях внутри ячеек
     (кроме контурных — там разброс подавлен).
  5. Холст сохраняется в JPEG с quality=95 и subsampling=0.

ПОДСКАЗКИ ПО КАЧЕСТВУ
---------------------
  * Символы «слиплись» в кашу   → увеличьте --scale 2..3 или -f.
  * Мало контуров                → уменьшите -e до 0.06–0.10.
  * Слишком много штрихов        → увеличьте -e до 0.20–0.30.
  * Хочется чистого пуантилизма  → -j 0.6 --no-edges.
  * Светлый исходник тёмный      → --bg white.
  * Нужен предсказуемый результат → -s 42 (или любое другое число).

ЛИЦЕНЗИЯ
--------
  Публичное достояние (CC0). Делайте что хотите.
"""

import argparse
import math
import random
from PIL import Image, ImageDraw, ImageFont


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


# ------------------------------------------------------------------
def lum(rgb):
    r, g, b = rgb
    return (0.299*r + 0.587*g + 0.114*b) / 255.0


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
    try:
        return ImageFont.load_default()
    except Exception:
        raise RuntimeError("Не удалось загрузить ни один шрифт")


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
    return result


# ------------------------------------------------------------------
# Детекция краёв
# ------------------------------------------------------------------
def detect_edges(small):
    """
    Sobel на уменьшенном изображении. Возвращает два массива:
      mag[y][x] — модуль градиента (0..~1442)
      ang[y][x] — направление градиента в градусах, [0, 180)
    Направление ГРАДИЕНТА перпендикулярно направлению КРАЯ.
    """
    w, h = small.size
    px = small.load()
    gray = [[0.0]*w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            gray[y][x] = (299*r + 587*g + 114*b) / 1000.0

    mag = [[0.0]*w for _ in range(h)]
    ang = [[0.0]*w for _ in range(h)]

    for y in range(1, h-1):
        for x in range(1, w-1):
            gx = (gray[y-1][x+1] + 2*gray[y][x+1] + gray[y+1][x+1]
                  - gray[y-1][x-1] - 2*gray[y][x-1] - gray[y+1][x-1])
            gy = (gray[y+1][x-1] + 2*gray[y+1][x] + gray[y+1][x+1]
                  - gray[y-1][x-1] - 2*gray[y-1][x] - gray[y-1][x+1])
            mag[y][x] = math.hypot(gx, gy)
            # atan2(gy,gx) % 180: даёт направление градиента [0..180)
            ang[y][x] = math.degrees(math.atan2(gy, gx)) % 180.0

    return mag, ang


def edge_glyph(angle_deg, strength):
    """
    По направлению градиента и силе края возвращает символ.
    Градиент ⊥ краю, поэтому:
      градиент ~0°   → край вертикальный  → '|'
      градиент ~45°  → край диагональный /' → '/'
      градиент ~90°  → край горизонтальный → '--' / '__'
      градиент ~135° → край диагональный \\ → '\\'
    """
    a = angle_deg
    if a < 22.5 or a >= 157.5:
        return '|'
    if a < 67.5:
        return '/'
    if a < 112.5:
        return '__' if strength > 1.6 else '--'   # сильная горизонталь — «жирная»
    return '\\'


# ------------------------------------------------------------------
def render(inp, out, font_size, jitter, scale, bg_choice, seed,
           use_edges=True, edge_threshold=0.12):
    random.seed(seed)
    src = Image.open(inp).convert("RGB")
    W, H = src.size

    W2 = max(1, int(W * scale))
    H2 = max(1, int(H * scale))

    cell_h = font_size
    cell_w = max(3, int(round(font_size * 0.6)))
    cw = max(1, W2 // cell_w)
    ch = max(1, H2 // cell_h)

    small = src.resize((cw, ch), Image.LANCZOS)
    px = small.load()

    bg_rgb = (0, 0, 0) if bg_choice == "black" else (255, 255, 255)
    options = build_options(bg_rgb)
    choices = dither(small, bg_rgb, options)

    # --- края -----------------------------------------------
    if use_edges:
        mag, ang = detect_edges(small)
        max_mag = max((max(row) for row in mag), default=0.0) or 1.0
        norm_mag = [[m / max_mag for m in row] for row in mag]
    else:
        norm_mag = None
        ang = None

    canvas = Image.new("RGB", (W2, H2), bg_rgb)
    draw = ImageDraw.Draw(canvas)
    font = find_font(font_size)

    for cy in range(ch):
        for cx in range(cw):
            pi, fill_glyph, k, _ = choices[cy][cx]
            color = PALETTE[pi]
            bx = cx * cell_w
            by = cy * cell_h

            # ---- 1. КОНТУР объекта -------------------------
            if use_edges and norm_mag[cy][cx] > edge_threshold:
                glyph = edge_glyph(ang[cy][cx], norm_mag[cy][cx])
                # контур — почти без «дрожания», иначе линия рвётся
                dx = int(random.uniform(-0.10, 0.10) * cell_w)
                dy = int(random.uniform(-0.10, 0.10) * cell_h)
                # подчёркивание прижато к низу шрифта — приподнимем
                if glyph.startswith('_'):
                    dy -= cell_h // 3
                xy = (bx + dx, by + dy)
                try:
                    draw.text(xy, glyph, font=font, fill=color, anchor="lt")
                except (TypeError, ValueError):
                    draw.text(xy, glyph, font=font, fill=color)
                continue

            # ---- 2. ЗАЛИВКА внутри объекта -----------------
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

    canvas.save(out, "JPEG", quality=95, subsampling=0)


def main():
    ap = argparse.ArgumentParser(
        description="JPEG → JPEG из ASCII-символов (16 цветов, дизеринг, "
                    "произвольные позиции + контурная обводка объектов).")
    ap.add_argument("input",  help="входной JPEG/PNG")
    ap.add_argument("output", help="выходной файл, например out.jpg")
    ap.add_argument("-f", "--font-size", type=int, default=None,
                    help="размер шрифта в пикселях")
    ap.add_argument("-j", "--jitter", type=float, default=0.40,
                    help="разброс позиции символа внутри ячейки (0 = решётка)")
    ap.add_argument("-e", "--edge-threshold", type=float, default=0.12,
                    help="порог обнаружения края 0..1 (меньше — больше контуров)")
    ap.add_argument("--no-edges", action="store_true",
                    help="отключить контурную обводку")
    ap.add_argument("--scale", type=float, default=1.0,
                    help="масштаб выходного изображения")
    ap.add_argument("--bg", choices=["black", "white"], default="black",
                    help="цвет подложки")
    ap.add_argument("-s", "--seed", type=int, default=42,
                    help="зерно генератора случайных позиций")
    args = ap.parse_args()

    src = Image.open(args.input)
    W, H = src.size
    fs = args.font_size or max(8, min(W, H) // 40)

    render(args.input, args.output, fs, args.jitter,
           args.scale, args.bg, args.seed,
           use_edges=not args.no_edges,
           edge_threshold=args.edge_threshold)
    print(f"Готово: {args.output}  (шрифт {fs}px, "
          f"jitter={args.jitter}, фон={args.bg}, "
          f"контуры={'выкл' if args.no_edges else args.edge_threshold})")


if __name__ == "__main__":
    main()
