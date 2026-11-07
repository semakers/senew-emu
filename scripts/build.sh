#!/bin/sh
# Build qemu-system-arm with the W580 machine model.
# Construye qemu-system-arm con el modelo de máquina W580.
#
#   scripts/build.sh [--static]
#
# Environment / variables de entorno:
#   JOBS       parallel jobs (default: 2; use 3-4 on a quiet machine)       trabajos en paralelo
#   WORK       working directory (default: ./build)                          directorio de trabajo
#   QEMU_TAR   path to an already downloaded qemu-10.2.2.tar.xz (skips the download)
#   NICE       nice level for ninja (default 10; set to 0 to disable)
#   PYTHON     python used by QEMU's configure (needs >= 3.11, or the tomli module; e.g. /opt/homebrew/bin/python3.12 on macOS)
#
# Result / resultado:  $WORK/qemu-build/qemu-system-arm   (also copied to $WORK/qemu-system-arm)
#
# Only the stock QEMU 10.2.2 tarball (checked against its sha256) plus the files in qemu/hw/arm/ are used.
# Solo se usa el tarball oficial de QEMU 10.2.2 (comprobado con su sha256) más los ficheros de qemu/hw/arm/.
set -eu

QEMU_VERSION=10.2.2
QEMU_SHA256=784b296ff29c1417aa72323abcb2d2ea9ab9771724f577dcd785c3b04f21e176
QEMU_URL="https://download.qemu.org/qemu-${QEMU_VERSION}.tar.xz"

STATIC=
for a in "$@"; do
    case "$a" in
        --static) STATIC=--static ;;
        -h|--help) sed -n '2,19p' "$0"; exit 0 ;;
        *) echo "unknown option: $a" >&2; exit 2 ;;
    esac
done

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
WORK=${WORK:-$ROOT/build}
JOBS=${JOBS:-2}
NICE=${NICE:-10}
TAR=${QEMU_TAR:-$WORK/qemu-${QEMU_VERSION}.tar.xz}
SRC=$WORK/qemu-${QEMU_VERSION}
BLD=$WORK/qemu-build
mkdir -p "$WORK"

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{print $1}'
    else shasum -a 256 "$1" | awk '{print $1}'; fi
}

# 1. get and verify the tarball / obtener y verificar el tarball
if [ ! -f "$TAR" ]; then
    echo ">> downloading $QEMU_URL"
    if command -v curl >/dev/null 2>&1; then curl -fL --retry 3 -o "$TAR.part" "$QEMU_URL"
    else wget -O "$TAR.part" "$QEMU_URL"; fi
    mv "$TAR.part" "$TAR"
fi
GOT=$(sha256_of "$TAR")
if [ "$GOT" != "$QEMU_SHA256" ]; then
    echo "sha256 mismatch for $TAR" >&2
    echo "  expected $QEMU_SHA256" >&2
    echo "  got      $GOT" >&2
    exit 1
fi
echo ">> sha256 ok"

# 2. unpack (once) / desempaquetar (una vez)
if [ ! -f "$SRC/configure" ]; then
    echo ">> unpacking"
    tar -xf "$TAR" -C "$WORK"
fi

# 3. overlay our machine / superponer nuestra máquina
cp "$ROOT"/qemu/hw/arm/w580.c "$ROOT"/qemu/hw/arm/*.inc "$SRC/hw/arm/"
grep -Fq "arm_ss.add(files('w580.c'))" "$SRC/hw/arm/meson.build" || \
    printf "\narm_ss.add(files('w580.c'))\n" >> "$SRC/hw/arm/meson.build"

# 4. configure: arm-softmmu only, nothing that is not needed / solo arm-softmmu, sin nada superfluo
if [ ! -f "$BLD/build.ninja" ] || [ "$(cat "$BLD/.w580-static" 2>/dev/null || true)" != "$STATIC" ]; then
    rm -rf "$BLD"; mkdir -p "$BLD"
    ( cd "$BLD" && "$SRC/configure" --target-list=arm-softmmu $STATIC ${PYTHON:+--python="$PYTHON"} \
        --disable-docs --disable-werror --disable-guest-agent --disable-tools \
        --disable-user --disable-sdl --disable-gtk --disable-opengl --disable-vnc \
        --disable-slirp --disable-capstone --disable-plugins --without-default-devices \
        --audio-drv-list= --disable-kvm --disable-xen --disable-curl --disable-gnutls \
        --disable-curses --disable-libusb --disable-usb-redir --disable-vhost-user \
        --disable-vhost-net --disable-vhost-kernel --disable-libssh --disable-spice \
        --disable-png --disable-vde --disable-brlapi --disable-linux-aio \
        --disable-linux-io-uring --disable-cap-ng --disable-seccomp --disable-zstd \
        --disable-debug-info ) > "$WORK/configure.log" 2>&1 || { tail -40 "$WORK/configure.log" >&2; exit 1; }
    printf '%s' "$STATIC" > "$BLD/.w580-static"
fi

# 5. build only the one binary / compilar solo el binario
echo ">> ninja -j$JOBS qemu-system-arm"
cd "$BLD"
if [ "$NICE" != 0 ] && command -v nice >/dev/null 2>&1; then
    nice -n "$NICE" ninja -j "$JOBS" qemu-system-arm
else
    ninja -j "$JOBS" qemu-system-arm
fi
cp "$BLD/qemu-system-arm" "$WORK/qemu-system-arm"
echo ">> done: $WORK/qemu-system-arm"
"$WORK/qemu-system-arm" -M help | grep -i w580 || true
