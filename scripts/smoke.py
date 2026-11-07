#!/usr/bin/env python3
"""Smoke test: boot the W580 machine with a flash image, wait, take a screendump over QMP, quit.
Prueba de humo: arranca la máquina W580 con una imagen de flash, espera, hace un screendump por QMP y sale.

    scripts/smoke.py QEMU FLASH [--seconds 6] [--expect-picture] [--out shot.ppm] [--opts estricto=on,...]

FLASH is a flat 64 MiB NOR image (a blank 0xFF one is enough to test that the machine starts and answers QMP).
--expect-picture: fail if the screen is entirely black (use it with a real senew image).
Python standard library only.
"""
import argparse, hashlib, json, os, socket, subprocess, sys, tempfile, time


class Qmp:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.connect(path)
        self.f = self.s.makefile('rw')
        self.f.readline()                      # greeting
        self.cmd('qmp_capabilities')

    def cmd(self, name, **args):
        self.f.write(json.dumps({'execute': name, 'arguments': args}) + '\n')
        self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if 'return' in r or 'error' in r:
                return r


def ppm_stats(path):
    d = open(path, 'rb').read()
    magic, dims, _maxv, px = d.split(b'\n', 3)
    w, h = map(int, dims.split())
    px = px[:w * h * 3]
    non_black = sum(1 for i in range(0, len(px), 3) if px[i] or px[i + 1] or px[i + 2])
    return w, h, non_black, hashlib.sha256(px).hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('qemu'); ap.add_argument('flash')
    ap.add_argument('--seconds', type=float, default=6)
    ap.add_argument('--expect-picture', action='store_true')
    ap.add_argument('--out', default=None)
    ap.add_argument('--opts', default='', help='extra machine options, e.g. estricto=on,panel-real=on')
    a = ap.parse_args()
    tmp = tempfile.mkdtemp(prefix='w580-smoke-')
    sock = os.path.join(tmp, 'qmp.sock')
    shot = a.out or os.path.join(tmp, 'screen.ppm')
    cmd = [a.qemu, '-name', 'w580-smoke', '-M', 'w580,i2c=0.0x4a:1.0x40,gpio-in=2.6=0' + (',' + a.opts if a.opts else ''), '-bios', a.flash,
           '-display', 'none', '-serial', 'none', '-monitor', 'none', '-qmp', 'unix:%s,server=on,wait=off' % sock]
    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    ok = False
    try:
        t0 = time.time()
        while not os.path.exists(sock):
            if p.poll() is not None or time.time() - t0 > 20:
                sys.exit('QEMU did not start: ' + (p.stderr.read().decode(errors='replace')[:500] if p.poll() is not None else 'timeout'))
            time.sleep(0.1)
        q = Qmp(sock)
        time.sleep(a.seconds)
        st = q.cmd('query-status')['return']
        print('status:', st)
        pant = q.cmd('qom-get', path='/machine', property='pantalla').get('return')
        print('pantalla property:', pant)
        r = q.cmd('screendump', filename=shot)
        nb = 0
        if 'error' in r:
            if a.expect_picture:
                sys.exit('screendump failed: %s' % r['error'])
            print('screendump: %s (ignored: no picture expected)' % r['error'])
        else:
            w, h, nb, digest = ppm_stats(shot)
            print('screen %dx%d, non-black pixels: %d, sha256(pixels)=%s' % (w, h, nb, digest))
        if st.get('status') != 'running':
            sys.exit('machine is not running')
        if a.expect_picture and nb == 0:
            sys.exit('FAIL: the screen is black')
        ok = True
        q.cmd('quit')
    finally:
        try:
            p.wait(timeout=5)
        except Exception:
            p.kill()
    print('OK' if ok else 'FAIL')


if __name__ == '__main__':
    main()
