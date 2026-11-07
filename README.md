# senew-emu — a QEMU machine model of the Sony Ericsson W580i

*Español más abajo — [ir a la versión en español](#español).*

> **This is a new, experimental project. Use it at your own risk.** The emulator is a model, not a phone: the real
> device can behave differently. senew is a young project too.

`senew-emu` adds one machine, `w580`, to QEMU 10.2.2: a model of the **W580i** handset (DB2020 system-on-chip, ARM926EJ-S
core, 64 MiB NOR flash, 240x320 LCD, keypad, accelerometer, LED chip, tone generator, PMU, RTC, Memory Stick, USB
device controller, Bluetooth UART). Its main job is to run **[senew](https://github.com/semakers/senew-w580)**, the open
operating system for that handset, in a browser (<https://semakers.com/probar/>), in tests and during development,
without touching a real phone.

It is a *machine model* written for this project: only the QEMU files that add the machine live here (`qemu/hw/arm/`),
plus a script that downloads the stock QEMU 10.2.2 tarball, checks its SHA-256, overlays the machine and builds only
`qemu-system-arm`.

## What this is NOT

* **It contains no firmware and no software from Sony Ericsson.** No firmware images, no flash or file-system dumps,
  no executables, graphics or fonts. You must bring your own flash image. The images built by senew
  (`make w580` → `build/flash-w580.bin`) are made entirely of senew and contain nothing from the manufacturer.
* It is not an exact replica of the silicon. Registers nobody has needed are not modelled (they are stored and read
  back, and logged the first times: "exploratory latch"). Timing is the host's, not the phone's, unless you use
  `-icount`.
* It does not emulate a camera, a DSP/audio codec, a radio or a network. The guest has no network at all.
* It is not affiliated with, endorsed by or supported by Sony or Sony Ericsson. *Sony Ericsson*, *W580i* and other
  names are trademarks of their respective owners and are used only to say which hardware the model is about.

One file of the development tree is **deliberately replaced here**: `w580-panel-tabla.inc`. In the development tree it is
generated from the display-driver configuration file of a real handset (the manufacturer's LCD initialisation
scripts). The public version keeps only the four panel-variant names and ids, with empty scripts. Nothing changes with
the default options; see [`NOTICE`](NOTICE).

## Build

You need a C toolchain, `ninja`, `pkg-config`, GLib 2.66+ and pixman headers, Python 3.11+ (or 3.9+ with `tomli`), and `curl` or
`wget`.

```sh
# Debian/Ubuntu
sudo apt install build-essential ninja-build pkg-config libglib2.0-dev libpixman-1-dev zlib1g-dev python3-venv xz-utils curl
# macOS (Homebrew)
brew install ninja pkg-config glib pixman python@3.12

scripts/build.sh                        # -> build/qemu-system-arm   (JOBS=4 to go faster, PYTHON=... if needed)
scripts/build.sh --static               # fully static binary, if your system has the static libraries
```

The script downloads `qemu-10.2.2.tar.xz` from `download.qemu.org`, verifies
`784b296ff29c1417aa72323abcb2d2ea9ab9771724f577dcd785c3b04f21e176`, copies `qemu/hw/arm/*` over `hw/arm/`, appends
`arm_ss.add(files('w580.c'))` to `hw/arm/meson.build` and builds `qemu-system-arm` for `arm-softmmu` with everything
that is not needed turned off (no graphics UI, no audio, no network, no PNG: screenshots are PPM).

Releases of this repository ship a Linux x86_64 binary built by GitHub Actions.

## Run

```sh
qemu-system-arm -M w580,i2c=0.0x4a:1.0x40,gpio-in=2.6=0 -bios flash.bin \
    -display none -serial none -monitor none \
    -qmp unix:/tmp/w580.qmp,server=on,wait=off
```

* `-bios flash.bin` is a flat **64 MiB NOR image** based at `0x44000000` (the machine refuses any other size). QEMU reads it
  once at start; the file is not written to. To keep the contents of the NOR when QEMU exits, add the machine option
  `nor-persistente=/path/out.bin`.
* `i2c=0.0x4a:1.0x40` presents the PMU companion chip and the LED chip; `gpio-in=2.6=0` leaves the slider open.
* Add `reset-real=on,apagado-real=on` so that "Restart" and "Switch off" in the phone's power menu really restart / power off
  the machine (QEMU exits on power-off). Keep the default `pmu-on-activa-baja` (on): with `off` the system believes the ON key
  is held down and switches itself off after ~13 s.
* **Power menu at boot.** The emulated PMU records "powered on by the ON key", so senew shows its power menu ("Apagar el
  teléfono / Bloquear el teclado / Reiniciar / Apagar la pantalla") a few seconds after every boot. Dismiss it with the right
  soft key: `qom-set /machine kp-cell 262` (press) then `qom-set /machine kp-cell 6` (release). The browser server of
  semakers.com does this automatically.
* There is no window: you drive it through QMP (a JSON socket) and read the screen with `screendump`.
  `scripts/smoke.py QEMU FLASH --expect-picture` boots an image, waits a few seconds, takes a screenshot and quits.
* `-icount shift=4,sleep=on,align=on` makes one instruction cost 16 ns of guest time (≈62.5 MIPS); without `-icount`
  the guest clock follows the host.

### Properties used by the browser emulator

Everything is a property of the machine object (`/machine`), driven with the QMP commands `qom-get` and `qom-set`:

| Property | Type | What |
|---|---|---|
| `kp-cell` | uint32 (set) | Keypad: `cell + 0x100` presses a key, `cell` releases it (cells below); `30` = power, `0xFF` releases everything. |
| `acel-x`, `acel-y` | uint8 | Accelerometer axes as the ADC reads them, 0..255 (rest ≈ 125/143). Setting one cancels `acel-senal`. |
| `gpio-in` | string | Forced GPIO inputs `PORT.BIT=0\|1\|-`, separated by `:`. `2.6=0` slider open, `2.6=1` closed. |
| `pantalla` | string (get) | Frames the display controller has been sent and number of non-black pixels of the last one: a cheap "did the screen change?" signal. |
| `pantalla-mmiacc` | string (get) | Copies made by the 2D accelerator and the panel cursor/window; use together with `pantalla`. |
| `luces-leds` | JSON string (get) | The six lights (screen backlight, two keypad zones, slider keypad, two RGB side bars) with level and colour now. |
| `tono-sucesos` | JSON string (get) | Changes of the tone generator since the last read, with virtual time (reading it drains the queue). |
| `virtual-time-ns` | int64 (get) | Virtual clock of the guest. |

Press / release a key (the value is `0x100 + cell` to press, `cell` to release; 284 = press cell 28, 28 = release it):

```json
{"execute":"qom-set","arguments":{"path":"/machine","property":"kp-cell","value":284}}
{"execute":"qom-set","arguments":{"path":"/machine","property":"kp-cell","value":28}}
```

Keypad cells: `1`=2 `2`=10 `3`=0 `4`=16 `5`=13 `6`=1 `7`=17 `8`=5 `9`=15 `0`=11 `*`=18 `#`=19 · left soft key=12, right soft
key=6 · up=27 down=25 left=26 right=29 centre (OK)=28 · back=7 clear=8 · Walkman=4 · volume −=21 volume +=24 ·
Activity=23 · power=30.

Take a screenshot: `{"execute":"screendump","arguments":{"filename":"/tmp/screen.ppm"}}` (a binary PPM, 240x320).
Full list of writable options: `qemu-system-arm -M w580,help`; the read-only properties above (`pantalla`, `pantalla-mmiacc`, `luces-leds`, `tono-sucesos`, `virtual-time-ns`) are not shown there: use the QMP command `qom-list` on `/machine`.

## Strict mode

`estricto=on` (with `estricto-aprender=FILE` to learn) is a **development aid**: it warns when software does something
the original firmware never does at the hardware level (unknown registers, out-of-order clock/PLL set-up, I2C
traffic to chips that are not there, cache-coherence hazards for the display, Memory Stick protocol violations…).
It is how senew was checked against the hardware before ever touching a phone. It is off by default and the browser
emulator does not use it. Its rule tables are inventories of register addresses and values *observed* at the
hardware level; they contain no manufacturer code. Part of the model (the NOR flash state machine, the PMU's ADC, the PLL
registers) lives in `w580-estricto.inc` for historical reasons and is always active.

## Layout

```
qemu/hw/arm/       the machine: w580.c + w580-*.inc (included by w580.c)
scripts/build.sh   download + verify + overlay + build qemu-system-arm
scripts/smoke.py   boot an image over QMP and take a screenshot
.github/workflows  CI: builds Linux x86_64 binaries, attaches them to releases
```

Comments in the sources are in Spanish (the project's working language) and mention internal notes
(`docs/…`, traces) that are not part of this repository.

## License

GPL-2.0-or-later, like QEMU. See [`LICENSE`](LICENSE) and [`NOTICE`](NOTICE) (credits: QEMU and **SEW800iEmu** by saup,
from which parts of the model were ported).

---

<a id="español"></a>

# senew-emu — un modelo de máquina QEMU del Sony Ericsson W580i

> **Este es un proyecto nuevo y experimental. Úsalo bajo tu propio riesgo.** El emulador es un modelo, no un teléfono:
> el aparato real puede comportarse de otra manera. senew también es un proyecto joven.

`senew-emu` añade una máquina, `w580`, a QEMU 10.2.2: un modelo del teléfono **W580i** (SoC DB2020, núcleo ARM926EJ-S, NOR
de 64 MiB, LCD de 240x320, teclado, acelerómetro, chip de LED, generador de tonos, PMU, RTC, Memory Stick, controlador
USB de dispositivo, UART Bluetooth). Sirve sobre todo para ejecutar **[senew](https://github.com/semakers/senew-w580)**,
el sistema operativo abierto de ese teléfono, en un navegador (<https://semakers.com/probar/>), en pruebas y durante el
desarrollo, sin tocar un teléfono de verdad.

Es un *modelo de máquina* escrito para este proyecto: aquí solo están los ficheros de QEMU que añaden la máquina
(`qemu/hw/arm/`) y un script que descarga el tarball oficial de QEMU 10.2.2, comprueba su SHA-256, superpone la máquina y
compila solo `qemu-system-arm`.

## Lo que NO es

* **No contiene firmware ni software de Sony Ericsson.** Ni imágenes de firmware, ni volcados de la flash o del sistema de
  ficheros, ni ejecutables, gráficos o tipos de letra. Tienes que traer tu propia imagen de flash. Las imágenes que
  construye senew (`make w580` → `build/flash-w580.bin`) son enteramente de senew y no llevan nada del fabricante.
* No es una réplica exacta del silicio. Los registros que nadie ha necesitado no se modelan (se guardan, se devuelven
  y se anotan las primeras veces: el «cerrojo exploratorio»). El tiempo es el del anfitrión, no el del teléfono, salvo
  que uses `-icount`.
* No emula cámara, DSP/códec de audio, radio ni red. El invitado no tiene red.
* No está afiliado, respaldado ni apoyado por Sony ni por Sony Ericsson. *Sony Ericsson*, *W580i* y otros nombres son
  marcas de sus respectivos dueños y se usan solo para decir de qué aparato trata el modelo.

Un fichero del árbol de desarrollo está **sustituido a propósito**: `w580-panel-tabla.inc`. En desarrollo se genera a
partir del fichero de configuración del controlador de pantalla de un teléfono real (los guiones de inicio del LCD del
fabricante). La versión pública solo conserva los cuatro nombres e identificadores de variante de panel, con guiones
vacíos. Con las opciones por omisión no cambia nada; mira [`NOTICE`](NOTICE).

## Compilar

Hace falta un compilador de C, `ninja`, `pkg-config`, las cabeceras de GLib 2.66+ y pixman, Python 3.11+ (o 3.9+ con
`tomli`) y `curl` o `wget`.

```sh
# Debian/Ubuntu
sudo apt install build-essential ninja-build pkg-config libglib2.0-dev libpixman-1-dev zlib1g-dev python3-venv xz-utils curl
# macOS (Homebrew)
brew install ninja pkg-config glib pixman python@3.12

scripts/build.sh                        # -> build/qemu-system-arm   (JOBS=4 para ir más rápido, PYTHON=... si hace falta)
scripts/build.sh --static               # binario estático, si el sistema tiene las bibliotecas estáticas
```

El script descarga `qemu-10.2.2.tar.xz` de `download.qemu.org`, comprueba
`784b296ff29c1417aa72323abcb2d2ea9ab9771724f577dcd785c3b04f21e176`, copia `qemu/hw/arm/*` sobre `hw/arm/`, añade
`arm_ss.add(files('w580.c'))` a `hw/arm/meson.build` y compila `qemu-system-arm` para `arm-softmmu` con todo lo
innecesario desactivado (sin interfaz gráfica, sin audio, sin red, sin PNG: las capturas son PPM).

Las *releases* de este repositorio traen un binario de Linux x86_64 compilado por GitHub Actions.

## Ejecutar

```sh
qemu-system-arm -M w580,i2c=0.0x4a:1.0x40,gpio-in=2.6=0 -bios flash.bin \
    -display none -serial none -monitor none \
    -qmp unix:/tmp/w580.qmp,server=on,wait=off
```

* `-bios flash.bin` es una **imagen plana de NOR de 64 MiB** con base en `0x44000000` (la máquina rechaza otro tamaño).
  QEMU la lee una vez al arrancar y no escribe en el fichero. Para conservar el contenido de la NOR al salir, añade la
  opción de máquina `nor-persistente=/ruta/salida.bin`.
* `i2c=0.0x4a:1.0x40` presenta el chip compañero del PMU y el chip de LED; `gpio-in=2.6=0` deja el deslizador abierto.
* Añade `reset-real=on,apagado-real=on` para que «Reiniciar» y «Apagar» del menú de encendido del teléfono reinicien / apaguen de
  verdad la máquina (QEMU termina al apagar). Deja `pmu-on-activa-baja` por omisión (on): con `off` el sistema cree que la tecla de
  encendido está mantenida y se apaga solo a los ~13 s.
* **Menú de encendido al arrancar.** El PMU emulado apunta «encendido por la tecla ON», así que senew muestra su menú de
  encendido unos segundos después de cada arranque. Se quita con la tecla de selección derecha: `qom-set /machine kp-cell 262`
  (pulsar) y `qom-set /machine kp-cell 6` (soltar). El servidor del navegador de semakers.com lo hace solo.
* No hay ventana: se maneja por QMP (un socket JSON) y la pantalla se lee con `screendump`.
  `scripts/smoke.py QEMU FLASH --expect-picture` arranca una imagen, espera unos segundos, hace una captura y sale.
* `-icount shift=4,sleep=on,align=on` hace que cada instrucción cueste 16 ns del tiempo del invitado (≈62,5 MIPS); sin
  `-icount` el reloj del invitado sigue al del anfitrión.

### Propiedades que usa el emulador del navegador

Todo son propiedades del objeto de máquina (`/machine`), con los comandos QMP `qom-get` y `qom-set`:

| Propiedad | Tipo | Qué es |
|---|---|---|
| `kp-cell` | uint32 (escribir) | Teclado: `celda + 0x100` pulsa, `celda` suelta (celdas abajo); `30` = encendido, `0xFF` suelta todo. |
| `acel-x`, `acel-y` | uint8 | Ejes del acelerómetro como los lee el ADC, 0..255 (reposo ≈ 125/143). Fijar uno cancela `acel-senal`. |
| `gpio-in` | cadena | Entradas de GPIO forzadas `PUERTO.BIT=0\|1\|-` separadas por `:`. `2.6=0` deslizador abierto, `2.6=1` cerrado. |
| `pantalla` | cadena (leer) | Cuadros enviados al controlador de pantalla y píxeles no negros del último: señal barata de «¿cambió la pantalla?». |
| `pantalla-mmiacc` | cadena (leer) | Copias del acelerador 2D y cursor/ventana del panel; se usa junto con `pantalla`. |
| `luces-leds` | cadena JSON (leer) | Las seis luces (retroiluminación, dos zonas del teclado, teclado del deslizador, dos barras RGB laterales) con su nivel y color de ahora. |
| `tono-sucesos` | cadena JSON (leer) | Cambios del generador de tonos desde la última lectura, con tiempo virtual (leerla vacía la cola). |
| `virtual-time-ns` | int64 (leer) | Reloj virtual del invitado. |

Pulsar y soltar una tecla (`0x100 + celda` pulsa, `celda` suelta): son los dos mensajes JSON de arriba (284 = pulsa la
celda 28, 28 = la suelta).

Celdas del teclado: `1`=2 `2`=10 `3`=0 `4`=16 `5`=13 `6`=1 `7`=17 `8`=5 `9`=15 `0`=11 `*`=18 `#`=19 · tecla de selección
izquierda=12, derecha=6 · arriba=27 abajo=25 izquierda=26 derecha=29 centro (OK)=28 · atrás=7 borrar=8 · Walkman=4 ·
volumen −=21 volumen +=24 · Actividad=23 · encendido=30.

Captura: `{"execute":"screendump","arguments":{"filename":"/tmp/pantalla.ppm"}}` (un PPM binario de 240x320). Todas las
opciones escribibles: `qemu-system-arm -M w580,help`; las propiedades de solo lectura de arriba (`pantalla`, `pantalla-mmiacc`, `luces-leds`, `tono-sucesos`, `virtual-time-ns`) no salen ahí: usa el comando QMP `qom-list` sobre `/machine`.

## Modo estricto

`estricto=on` (con `estricto-aprender=FICHERO` para aprender) es una **ayuda de desarrollo**: avisa cuando el software hace
a nivel de hardware algo que el firmware original no hace nunca (registros desconocidos, secuencia de reloj/PLL
fuera de orden, tráfico I2C a chips que no están, riesgos de coherencia de caché de la pantalla, violaciones del
protocolo del Memory Stick…). Así se comprobó senew contra el hardware antes de tocar un teléfono. Está apagado por omisión y
el emulador del navegador no lo usa. Sus tablas de reglas son inventarios de direcciones y valores de registros
*observados* a nivel de hardware; no contienen código del fabricante. Una parte del modelo (la máquina de estados de la
NOR, el ADC del PMU, los registros del PLL) vive en `w580-estricto.inc` por razones históricas y está siempre activa.

## Estructura

```
qemu/hw/arm/       la máquina: w580.c + w580-*.inc (los incluye w580.c)
scripts/build.sh   descargar + verificar + superponer + compilar qemu-system-arm
scripts/smoke.py   arrancar una imagen por QMP y hacer una captura
.github/workflows  CI: compila binarios Linux x86_64 y los adjunta a las releases
```

Los comentarios del código están en español (la lengua de trabajo del proyecto) y mencionan notas internas (`docs/…`,
trazas) que no forman parte de este repositorio.

## Licencia

GPL-2.0-or-later, como QEMU. Mira [`LICENSE`](LICENSE) y [`NOTICE`](NOTICE) (créditos: QEMU y **SEW800iEmu** de saup, de
donde se portaron partes del modelo).
