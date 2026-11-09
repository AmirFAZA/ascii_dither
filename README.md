# ascii_dither

**JPEG/PNG → ASCII-арт в 16 цветах ANSI.**

Две реализации одной идеи:

- **`ascii_dither.py`** — исходная версия на Python + Pillow (просто запустить, без компиляции).
- **C-порт** (`src/ascii_dither.c`) — быстрый CLI и Win32 GUI, один exe без внешних
  зависимостей (stb уже внутри).

<p align="center">
  <img src="example.jpg" width="700" alt="Пример результата: изображение, собранное из ASCII-символов">
</p>

---

## Содержание

- [Возможности](#возможности)
- [Быстрый старт](#быстрый-старт)
- [Python-версия](#python-версия)
- [Сборка C-версии](#сборка-c-версии)
- [CLI (C)](#cli-c)
  - [Примеры](#примеры)
  - [Флаги](#флаги)
- [GUI (Windows)](#gui-windows)
- [Параметр Scale](#параметр-scale)
- [Как это работает](#как-это-работает)
- [Структура репозитория](#структура-репозитория)
- [Ограничения](#ограничения)
- [Лицензия](#лицензия)

---

## Возможности

- **16 цветов ANSI/VGA** (классическая консольная палитра) + дизеринг **Флойда–Штейнберга**.
- **Контуры (Sobel)**: направленные символы `- | / \ _` и обводка объектов.
- Подбор символа по «плотности» ячейки и случайный **jitter** позиции внутри ячейки.
- **Многопоточный** рендер в C-версии (по умолчанию — все ядра).
- В C-версии — только `stb` (лежит в `src/`), никаких DLL и Python.
- **GUI (Windows)**: авто-превью, зум по наведению, экспорт в полном разрешении.
- **CLI** с логами, таймингами и настраиваемым фоном.

## Быстрый старт

**Windows — готовый exe:** скачайте `ascii_dither.exe` из
[Releases](../../releases) и запустите двойным кликом (откроется GUI).
С аргументами тот же exe работает как CLI.

**Python:**

```bat
pip install pillow
python ascii_dither.py photo.jpg out.jpg
```

**Собрать C-версию:**

```bat
build.bat
dist\ascii_dither.exe photo.jpg out.jpg
```

## Python-версия

Файл `ascii_dither.py` — оригинальная реализация. Требуется Python 3 и Pillow.

```bat
pip install pillow
python ascii_dither.py <input> <output> [опции]
```

```bat
:: базовый запуск
python ascii_dither.py photo.jpg out.jpg

:: крупный символ, апскейл ×2, светлая подложка
python ascii_dither.py photo.jpg out.jpg -f 20 --scale 2 --bg white

:: без контуров
python ascii_dither.py photo.jpg out.jpg --no-edges

:: фиксированный seed — воспроизводимый результат
python ascii_dither.py photo.jpg out.jpg -s 42
```

| Флаг | По умолчанию | Описание |
|---|---|---|
| `-f`, `--font-size N` | `max(8, min(W,H)//40)` | Высота ячейки в пикселях |
| `-j`, `--jitter F` | `0.40` | Разброс позиции символа в ячейке, `0..1` |
| `-e`, `--edge-threshold F` | `0.12` | Порог Sobel для контуров, `0..1` |
| `--no-edges` | выкл. | Отключить контуры |
| `--scale K` | `1.0` | Масштаб выходного холста: `W·K × H·K` |
| `--bg {black,white}` | `black` | Цвет подложки |
| `-s`, `--seed N` | `42` | Зерно jitter |
| `-q`, `--quiet` | выкл. | Без логов |
| `-h`, `--help` | — | Справка |

Выход — JPEG quality 95, subsampling=0. Логи идут в stderr.

## Сборка C-версии

**Требования**

| Что | Зачем |
|---|---|
| MinGW-w64 GCC (WinLibs, MSYS2, TDM) | компиляция |
| Python 3 *(необязательно)* | встраивание манифеста в exe |

**Windows:**

```bat
build.bat
:: или явно указать компилятор
build.bat C:\mingw64\bin\gcc.exe
```

Вручную:

```bat
gcc -O2 -std=c99 -mwindows -static -static-libgcc -Isrc ^
    -o dist\ascii_dither.exe src\ascii_dither.c ^
    -lcomctl32 -lgdi32 -lcomdlg32 -luser32 -lshell32 -lm
python tools\embed_manifest.py dist\ascii_dither.exe res\ascii_dither.manifest
```

`-static -static-libgcc` даёт автономный exe: рядом не нужны DLL от MinGW.

**Манифест.** `tools/embed_manifest.py` кладёт `res/ascii_dither.manifest`
в PE-ресурс `RT_MANIFEST` (id 1): современные визуальные стили Windows +
DPI-awareness. Скрипт необязателен — без манифеста приложение работает,
но с классическими контролами.

**Linux / macOS (только CLI, GUI исключён):**

```sh
./build.sh
dist/ascii_dither photo.jpg out.jpg
```

## CLI (C)

```
ascii_dither.exe <input> <output> [options]
ascii_dither.exe --gui
```

`input` — JPEG/PNG/BMP/TGA (всё, что умеет `stb_image`), `output` — всегда JPEG q95.

### Примеры

```bat
:: базовый рендер
ascii_dither.exe photo.jpg out.jpg

:: крупнее и детальнее
ascii_dither.exe photo.jpg out.jpg --scale 2 -f 12

:: чёрная подложка, меньше «шума», без контуров
ascii_dither.exe photo.jpg out.jpg --bg black -j 0.2 --no-edges

:: свой цвет подложки из палитры ANSI (4 = navy), 8 потоков, фиксированный seed
ascii_dither.exe photo.jpg out.jpg --bg 4 -t 8 -s 123

:: тихий режим (без логов)
ascii_dither.exe photo.jpg out.jpg -q

:: открыть GUI
ascii_dither.exe --gui
```

### Флаги

| Флаг | Аргумент | По умолчанию | Описание |
|---|---|---|---|
| `-f`, `--font-size` | `N` | `0` (авто) | Высота ячейки в пикселях. `0` → `min(W, H) / 40` |
| `-j`, `--jitter` | `F` | `0.40` | Разброс позиции символа внутри ячейки, `0..1` |
| `-e`, `--edge-threshold` | `F` | `0.12` | Порог Sobel для контурных символов, `0..1` |
| `--no-edges` | — | выкл. | Отключить контуры |
| `--scale` | `K` | `1.0` | Масштаб выходного холста: `W·K × H·K` |
| `--bg` | `MODE` | `auto` | `auto`, `black`, `white` или индекс цвета ANSI `0..15` |
| `-s`, `--seed` | `N` | `42` | Зерно для jitter (воспроизводимость) |
| `-t`, `--threads` | `N` | `0` (авто) | Потоков рендера; `0` — все ядра |
| `-q`, `--quiet` | — | выкл. | Не печатать логи |
| `--gui` | — | — | Открыть графическое окно |
| `-h`, `--help` | — | — | Справка |

Цвета палитры для `--bg`: `0` black, `1` maroon, `2` green, `3` olive, `4` navy,
`5` purple, `6` teal, `7` silver, `8` gray, `9` red, `10` lime, `11` yellow,
`12` blue, `13` fuchsia, `14` aqua, `15` white.

Пример лога:

```
============================================================
ascii_dither: photo.jpg -> out.jpg
============================================================
input: photo.jpg
  source: 1061x1280
  canvas 1061x1280, cells 66x49 (16x26 px), threads 12
  auto background: gray (ANSI 8, rgb 128,128,128)
  dithering...
  edges (Sobel, 12 threads): done
  font: C:/Windows/Fonts/consola.ttf
  rendering...
  render done 1061x1280
  saving out.jpg (JPEG q95)...
done: out.jpg
```

## GUI (Windows)

Окно открывается без аргументов или с `--gui`. Превью пересобирается само
(debounce 450 мс после изменения настроек) — кнопки «Render» нет.

| Элемент | Что делает |
|---|---|
| **Input image** | Путь к исходнику (`..` — выбор файла). Загружается и превью-миниатюра |
| **Source** | Миниатюра исходника с сохранением пропорций |
| **Result** | Результат; при наведении — зум ×2.8, внизу подпись `preview … export …` |
| **Output (Export)** | Путь для экспорта (`..` — выбор файла) |
| **Font / Jitter / Edge / Scale / Seed / Threads** | Слайдер + поле ручного ввода значения |
| **BG / No edges** | Цвет подложки (`auto (ANSI)`, `black`, `white`) и отключение контуров |
| **Export** | Полноразмерный рендер в JPEG q95 (не превью) |
| **Log** | Тайминги, размер холста, сетка ячеек, найденный шрифт |

Превью ограничено 900 px по большей стороне (скорость), экспорт — полное
разрешение `W·scale × H·scale`.

## Параметр Scale

`Scale` — множитель **разрешения** холста (как `--scale` в CLI): ячейки и символы
того же размера, но их становится больше, т.е. растёт детализация.

| Scale | Выход для 1061×1280 | Ячеек (CLI, font auto) |
|---|---|---|
| 1 | 1061×1280 | 66×49 |
| 2 | 2122×2560 | 132×98 |
| 3 | 3183×3840 | 198×147 |
| 4 | 4244×5120 | 265×196 |

В GUI превью ужимается до 900 px, поэтому Scale виден как рост плотности
(от крупных читаемых символов к мелкой фактуре), а фактический размер экспорта
показан подписью в панели Result.

## Как это работает

1. Изображение читается через `stb_image` (RGB) — или через Pillow в Python-версии.
2. Холст увеличивается в `scale` раз и разбивается на сетку ячеек `cw × ch`
   (размер ячейки — `font_size`, по умолчанию `min(W, H) / 40`).
3. Исходник уменьшается до `cw × ch` — по одной ячейке на пиксель.
4. Для каждой ячейки подбирается пара «цвет палитры + символ» с подходящей
   плотностью; ошибка распределяется по **Флойду–Штейнбергу**.
5. **Sobel** даёт магнитуду и угол края: на контурах рисуются направленные
   символы (`- | / \ _`), что даёт обводку объектов.
6. Символы растеризуются из TTF со случайным сдвигом (jitter); в C-версии
   строки холста рендерятся параллельно.
7. Результат кодируется в JPEG q95.

Шрифт ищется автоматически: `consola.ttf`, `cour.ttf`, `lucon.ttf`,
`DejaVuSansMono.ttf` (Windows) и стандартные пути DejaVu/Liberation/Ubuntu
Mono на Linux/macOS.

## Структура репозитория

```
.
├── ascii_dither.py             # исходная версия на Python (Pillow)
├── src/
│   ├── ascii_dither.c          # C-версия: палитра, дизеринг, Sobel, CLI, GUI
│   ├── stb_image.h             # чтение JPEG/PNG
│   ├── stb_image_resize.h      # уменьшение до сетки ячеек
│   ├── stb_image_write.h       # запись JPEG
│   └── stb_truetype.h          # растеризация глифов
├── res/
│   └── ascii_dither.manifest   # ComCtl32 v6, supportedOS, PerMonitorV2
├── tools/
│   └── embed_manifest.py       # встраивание RT_MANIFEST в exe (WinAPI)
├── build.bat                   # сборка для Windows
├── build.sh                    # сборка CLI на Linux/macOS
├── example.jpg                 # пример результата
├── LICENSE
└── README.md
```

## Ограничения

- **GUI только Windows** (Win32 API). CLI собирается и на Linux/macOS.
- Python-версии нужен Pillow; C-версия — автономный exe.
- Нужен моноширинный TTF; на Windows он есть всегда (`consola.ttf`).
- Выход — всегда JPEG q95. При `--scale 3..4` файлы крупные
  (для 1061×1280 при scale 4 — 4244×5120, ~20 МБ).
- Превью в GUI ограничено 900 px и не показывает 1:1 пиксели экспорта.
- Точный вид зависит от seed и jitter — при одинаковых параметрах результат
  воспроизводим.

## Лицензия

Проект распространяется по **CC0 1.0 Universal** — см. [LICENSE](LICENSE).

Сторонние компоненты: [stb](https://github.com/nothings/stb) (`stb_image.h`,
`stb_image_resize.h`, `stb_image_write.h`, `stb_truetype.h`) — public domain / MIT,
подробности в шапках файлов.
