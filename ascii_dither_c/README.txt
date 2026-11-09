ascii_dither.c — C-порт ascii_dither.py
======================================

Что это
-------
Конвертер JPEG/PNG → JPEG, рендерящий изображение ASCII-символами
из 16 стандартных ANSI-цветов, с дизерингом Флойда–Штейнберга,
детекцией контуров (Sobel) и логами прогресса. Порт оригинального
ascii_dither.py.

Состав папки
------------
  ascii_dither.c        — исходный код (C99)
  stb_image.h           — загрузка изображений (public domain)
  stb_image_resize.h    — масштабирование (public domain, deprecated API)
  stb_image_write.h     — сохранение JPEG/PNG/BMP (public domain)
  stb_truetype.h        — растеризация шрифтов TrueType (public domain)
  README.txt            — этот файл

Сборка
------
Требуется компилятор C (C99/C11). Заголовки stb лежат рядом — ничего
устанавливать не нужно (кроме моноширинного шрифта в системе).

MinGW-w64 / GCC / Clang (Windows или Linux/macOS):
  gcc -O2 -std=c99 -o ascii_dither.exe ascii_dither.c -lm

MSVC (Developer Command Prompt):
  cl /O2 ascii_dither.c

Примечание: на целевой машине, где готовилась эта папка, нативный
компилятор Windows (gcc/clang/cl) в PATH не обнаружен; поэтому здесь
лежит исходный код, готовый к сборке. Скомпилируйте командой выше
после установки MinGW-w64 (https://winlibs.com/) или Visual Studio
Build Tools.

Запуск
------
  ascii_dither.exe <input> <output> [опции]

Опции (совместимы с Python-версией):
  -f, --font-size N       размер шрифта, px (по умолчанию ~min(W,H)/40)
  -j, --jitter F          разброс позиции 0..1 (0.40)
  -e, --edge-threshold F  порог края 0..1 (0.12)
  --no-edges              отключить контуры
  --scale K               масштаб холста (1.0)
  --bg {black,white}      цвет подложки (black)
  -s, --seed N            зерно случайных позиций (42)
  -q, --quiet             подавить логи
  -h, --help              справка

Пример:
  ascii_dither.exe "C:\Users\Admin\Desktop\example.jpg" out.jpg -f 16 --scale 1.0

Отличия от Python-версии
------------------------
- Рендер шрифта: stb_truetype (растр в альфа-битмап + блендинг),
  а не PIL.ImageDraw. Визуально близко, возможны мелкие отличия в кернинге/хинтинге.
- Масштабирование: stb_image_resize (фильтр по умолчанию), в Python — LANCZOS.
- JPEG-сохранение: stb_image_write quality=95 (без параметра subsampling=0).
- Нужен установленный моноширинный TTF (Consolas/Courier/Lucida Console на Windows,
  DejaVu Sans Mono на Linux). Пути поиска совпадают с Python-версией.
