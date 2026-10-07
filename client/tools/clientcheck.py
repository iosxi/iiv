"""iiv-client の自動検証(iiv-server の合成した絵で。利用者の画面は写さない・入力は再現しない)。

    python tools/clientcheck.py

../server の iiv-server.exe と tools/iivcheck.py(起動・停止)を使う。クライアントは検証用の ini と
-dump / -exitafter / -idleexit で動かし、窓は前面に出さない(SW_SHOWNOACTIVATE)。
入力は PostMessage でクライアントの窓へ直接送る(利用者のキーボード・マウスは使わない)。
"""
import ctypes, os, re, subprocess, sys, time
from ctypes import wintypes as W
import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'iiv-client.exe')
TEST = os.path.join(ROOT, 'build', 'test')
sys.path.insert(0, os.path.join(ROOT, '..', 'server', 'tools'))
import iivcheck as t  # noqa: E402

ok = True
u = ctypes.windll.user32
u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))   # 窓の大きさを物理の画素で聞く(DPI 非対応だと拡大率で割った値になる)


def check(cond, label, detail=''):
    global ok
    print(f'  {label:40s} {"OK" if cond else "NG"}  {detail}')
    if not cond:
        ok = False


def psnr(a, b):
    m = np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2)
    return 99.0 if m == 0 else 10 * np.log10(255 * 255 / m)


def run_client(args, render='gpu', wait=40):
    ini = os.path.join(TEST, 'cc.ini')
    log = os.path.join(TEST, 'cc.log')
    with open(ini, 'w', encoding='utf-8') as f:
        f.write(f'[client]\nrender={render}\nfit=1\n[general]\nlog=1\n')
    if os.path.exists(log):
        os.remove(log)
    si = subprocess.STARTUPINFO()
    si.dwFlags = 1
    si.wShowWindow = 4
    p = subprocess.Popen([EXE, '-ini', ini, f'127.0.0.1:{t.PORT}', '-password', 'cc'] + args, startupinfo=si)
    try:
        p.wait(timeout=wait)
    except subprocess.TimeoutExpired:
        p.kill()
    return open(log, encoding='utf-8').read() if os.path.exists(log) else ''


def main():
    os.makedirs(TEST, exist_ok=True)
    ref_path = os.path.join(TEST, 'ref.bmp')

    print('[止まった絵: サーバーの元の絵と比べる]')
    for render in ('gpu', 'gdi'):
        t.start_server(['-testsrc', 'static', '-testdump', ref_path], password='cc')
        dump = os.path.join(TEST, f'static-{render}.bmp')
        try:
            if os.path.exists(dump):
                os.remove(dump)
            log = run_client(['-dump', dump, '-idleexit', '1500'], render)
        finally:
            t.stop_server()
        if not os.path.exists(dump):
            check(False, f'{render}: 絵を書いた', log[-300:])
            continue
        ref = np.asarray(Image.open(ref_path).convert('RGB'))
        got = np.asarray(Image.open(dump).convert('RGB'))
        # 合否は明るさ(Y)で見る。RGB は色差の 4:2:0 の間引きで、ClearType の色付きの文字の縁が崩れる分だけ低い
        Y = lambda x: 0.2126 * x[..., 0] + 0.7152 * x[..., 1] + 0.0722 * x[..., 2]
        whole, text = psnr(Y(ref), Y(got)), psnr(Y(ref[120:720, 40:940]), Y(got[120:720, 40:940]))   # 文字の欄
        rgbw, rgbt = psnr(ref, got), psnr(ref[120:720, 40:940], got[120:720, 40:940])
        dec = re.search(r'復号: .*', log)
        check(got.shape == ref.shape and whole >= 42 and text >= 36, f'{render}: 全体 / 文字の欄の PSNR(Y)',
              f'{whole:.1f} / {text:.1f} dB(RGB {rgbw:.1f} / {rgbt:.1f})  {dec[0] if dec else ""}')

    print('[動く絵: 300 フレームを受け取って描く]')
    for src, label in (([], '文字・四角・写真'), (['video'], '全面が毎フレーム変わる')):
        t.start_server(['-testsrc'] + src + ['-testfps', '60'], password='cc')
        try:
            log = run_client(['-exitafter', '300'])
        finally:
            t.stop_server()
        m = re.search(r'更新 (\d+) 回\(([\d.]+) 回/秒\)', log)
        bad = 'を復号できない' in log
        check(m and int(m[1]) >= 300 and not bad, label, f'{m[0] if m else "結果なし"}  復号の失敗 {"あり" if bad else "なし"}')

    print('[途中で画面の大きさが変わる(100 フレーム目で 1280x720)]')
    t.start_server(['-testsrc', '-testfps', '60', '-testresize', '100'], password='cc')
    dump = os.path.join(TEST, 'resize.bmp')
    try:
        if os.path.exists(dump):
            os.remove(dump)
        log = run_client(['-exitafter', '200', '-dump', dump])
    finally:
        t.stop_server()
    size = Image.open(dump).size if os.path.exists(dump) else None
    check(size == (1280, 720) and log.count('映像 ') >= 2, '新しい大きさで描き続ける', f'絵 {size}、映像の設定 {log.count("映像 ")} 回')

    print('[入力: 窓へ送ったキー・マウスがサーバーに届く]')
    t.start_server(['-testsrc', 'static'], password='cc')
    ini = os.path.join(TEST, 'cc.ini')
    with open(ini, 'w', encoding='utf-8') as f:
        f.write('[client]\nrender=gpu\nfit=1\n[general]\nlog=1\n')
    si = subprocess.STARTUPINFO()
    si.dwFlags = 1
    si.wShowWindow = 4
    p = subprocess.Popen([EXE, '-ini', ini, f'127.0.0.1:{t.PORT}', '-password', 'cc'], startupinfo=si)
    try:
        time.sleep(3)
        hw = []

        @ctypes.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
        def cb(h, l):
            pid = W.DWORD()
            u.GetWindowThreadProcessId(h, ctypes.byref(pid))
            cls = ctypes.create_unicode_buffer(64)
            u.GetClassNameW(h, cls, 64)
            if pid.value == p.pid and cls.value == 'iiv.Client.View':
                hw.append(h)
            return True
        u.EnumWindows(cb, 0)
        h = hw[0]
        rc = W.RECT()
        u.GetClientRect(h, ctypes.byref(rc))
        cx, cy = rc.right // 2, rc.bottom // 2         # 窓の真ん中 = 相手の (960, 540) あたり
        u.PostMessageW(h, 0x100, 0x41, (0x1E << 16) | 1)
        u.PostMessageW(h, 0x101, 0x41, (0x1E << 16) | 1 | (3 << 30))
        u.PostMessageW(h, 0x100, 0x25, (0x4B << 16) | 1 | (1 << 24))
        u.PostMessageW(h, 0x101, 0x25, (0x4B << 16) | 1 | (1 << 24) | (3 << 30))
        u.PostMessageW(h, 0x200, 0, (cy << 16) | cx)
        u.PostMessageW(h, 0x201, 1, (cy << 16) | cx)
        u.PostMessageW(h, 0x202, 0, (cy << 16) | cx)
        time.sleep(1)
        u.PostMessageW(h, 0x10, 0, 0)
        p.wait(timeout=10)
    finally:
        t.stop_server()
    log = t.server_log()
    keys = re.findall(r'\[dryrun-key\] (down|up) scan=(\w+) vk=(\w+)', log)
    check(keys == [('down', '1E', '41'), ('up', '1E', '41'), ('down', '14B', '25'), ('up', '14B', '25')],
          'キー: スキャン コードと拡張の印', str(keys))
    # ボタンを押す直前の移動で見る(本物のカーソルが新しく出た窓に重なっていると、その位置への移動が先に届く)
    before = log.split('flags=0002')[0]
    mvs = re.findall(r'\[dryrun\] mouse flags=C001 dx=(\d+) dy=(\d+)', before)
    mv = mvs[-1] if mvs else None
    if mv:
        x = int(mv[0]) * 1920 * 2 // 65536 // 2
        y = int(mv[1]) * 1080 * 2 // 65536 // 2
        check(abs(x - 960) <= 4 and abs(y - 540) <= 4 and 'flags=0002' in log and 'flags=0004' in log,
              'マウス: 窓の真ん中 = 相手の真ん中、左ボタン', f'({x},{y})')
    else:
        check(False, 'マウス', log[-300:])

    print('ALL OK' if ok else 'SOME NG')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
