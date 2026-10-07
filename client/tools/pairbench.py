"""iiv-server と iiv-client を組み合わせて、速さ・遅れ・CPU を測る(同じ PC の中)。

    python tools/pairbench.py [--src video|static|move] [--fps 0] [--frames 600] [--quality auto] [--render gpu|gdi]
                              [--audio off|quality|speed] [--realaudio]

--audio を付けると「音を鳴らす」を両方で有効にする。サーバーは -testaudio(合成した音)、
--realaudio なら鳴っている音を取り込む。クライアントは音量 0 で鳴らし(-audiomute)、
受け取った音を build/test/bench.wav に書く。

サーバーは ../server の iiv-server を -testsrc と検証用の ini(127.0.0.1:5999、パスワード bench)で動かす。
クライアントは -exitafter で決まったフレーム数を受けたら終わる。両方のプロセスの CPU 時間を測り、
クライアントのログの「検証の終わり」と、サーバーのログの 5 秒ごとの集計を出す。
move = 文字が流れ四角が動く絵(既定の -testsrc)。
"""
import argparse, ctypes, os, re, subprocess, sys, time
from ctypes import wintypes as W

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SROOT = os.path.join(ROOT, '..', 'server')
sys.path.insert(0, os.path.join(SROOT, 'tools'))
import iivcheck  # noqa: E402

TEST = os.path.join(ROOT, 'build', 'test')


def cpu_seconds(pid):
    k = ctypes.windll.kernel32
    h = k.OpenProcess(0x1000, False, pid)
    if not h:
        return None
    c, e, kt, ut = W.FILETIME(), W.FILETIME(), W.FILETIME(), W.FILETIME()
    k.GetProcessTimes(h, ctypes.byref(c), ctypes.byref(e), ctypes.byref(kt), ctypes.byref(ut))
    k.CloseHandle(h)
    f = lambda t: (t.dwHighDateTime << 32 | t.dwLowDateTime) / 1e7
    return f(kt) + f(ut)


def server_pid():
    out = subprocess.run(['powershell', '-c', '(Get-NetTCPConnection -State Listen -LocalPort 5999).OwningProcess'],
                         capture_output=True, text=True).stdout.strip()
    return int(out.split()[0]) if out else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--src', default='video', choices=['video', 'static', 'move'])
    ap.add_argument('--fps', type=int, default=0)
    ap.add_argument('--frames', type=int, default=600)
    ap.add_argument('--quality', default='auto')
    ap.add_argument('--render', default='gpu')
    ap.add_argument('--softenc', action='store_true')
    ap.add_argument('--audio', default='off', choices=['off', 'quality', 'speed'])
    ap.add_argument('--realaudio', action='store_true')
    a = ap.parse_args()
    os.makedirs(TEST, exist_ok=True)
    sargs = ['-testsrc'] + ([] if a.src == 'move' else [a.src]) + ['-testfps', str(a.fps)] + (['-softenc'] if a.softenc else [])
    if a.audio != 'off' and not a.realaudio:
        sargs.append('-testaudio')
    iivcheck.start_server(sargs, password='bench', extra='audio=1\n' if a.audio != 'off' else '')
    spid = server_pid()
    cini = os.path.join(TEST, 'bench.ini')
    log = os.path.join(TEST, 'bench.log')
    with open(cini, 'w', encoding='utf-8') as f:
        f.write(f'[client]\nquality={a.quality}\nrender={a.render}\n[general]\nlog=1\n')
    if os.path.exists(log):
        os.remove(log)
    si = subprocess.STARTUPINFO()
    si.dwFlags = 1
    si.wShowWindow = 4
    s0 = cpu_seconds(spid)
    t0 = time.perf_counter()
    wav = os.path.join(TEST, 'bench.wav')
    aargs = ['-audio', a.audio] + (['-audiomute', '-audiodump', wav] if a.audio != 'off' else [])
    p = subprocess.Popen([os.path.join(ROOT, 'iiv-client.exe'), '-ini', cini, '127.0.0.1:5999', '-password', 'bench',
                          '-exitafter', str(a.frames), '-log'] + aargs, startupinfo=si)
    c1 = None
    while p.poll() is None:
        c1 = cpu_seconds(p.pid) or c1
        time.sleep(0.05)
        if time.perf_counter() - t0 > 120:
            p.kill()
            break
    dt = time.perf_counter() - t0
    s1 = cpu_seconds(spid)
    iivcheck.stop_server()
    clog = open(log, encoding='utf-8').read() if os.path.exists(log) else ''
    m = re.search(r'検証の終わり: ([\d.]+) 秒で 更新 (\d+) 回\(([\d.]+) 回/秒\)、受信 (\d+) バイト\(([\d.]+) Mbps\)、受信と復号 平均 ([\d.]+)ms', clog)
    if not m:
        print('クライアントのログに結果が無い'); print(clog[-1500:]); return 1
    sec, n, rate, by, mbps, dec = float(m[1]), int(m[2]), float(m[3]), int(m[4]), float(m[5]), float(m[6])
    ccpu = (c1 or 0) / sec * 100
    scpu = (s1 - s0) / dt * 100 if s0 is not None and s1 is not None else float('nan')
    print(f'{a.src} {a.quality} {a.render}{" softenc" if a.softenc else ""} 音 {a.audio}: {rate:.1f} フレーム/秒  {mbps:.1f} Mbps  1 フレーム {by / n / 1024:.0f} KB  '
          f'復号 {dec:.2f}ms  クライアントの CPU {ccpu:.0f}%(1 フレーム {ccpu * 10 / rate:.1f}ms)  サーバーの CPU {scpu:.0f}%')
    for l in iivcheck.server_log().splitlines():
        if 'フレーム/秒' in l or '映像:' in l or '音:' in l or '音を' in l:
            print('  サーバー:', l[24:])
    for l in clog.splitlines():
        if '音:' in l:
            print('  クライアント:', l[24:])
    return 0


if __name__ == '__main__':
    sys.exit(main())
