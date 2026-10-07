"""「音を鳴らす」の検証(同じ PC の中。音は出さない)。

    python tools/audiocheck.py [--seconds 15]

サーバーを -testsrc video -testaudio(合成した音: 左 = 2 秒で 200Hz → 8kHz のスイープ、右 = 1kHz)で動かし、
クライアントを -audiomute(音量 0 で鳴らす)と -audiodump(受け取った音を WAV に書く)で動かす。

  1. 音質優先(PCM): WAV がサーバーの作った音と 1 サンプルずつ同じか(±1)、途中で欠けていないか
  2. 速度優先(AAC): 同じく SNR と欠け
  3. サーバーの「音を鳴らす」が切られている: クライアントに「切られている」が届き、サーバーは取り込みを始めない
  4. クライアントが「音を鳴らす」にしていない: サーバーは音を求められず、取り込みを始めない
  5. つないだまま切り替える(窓のメニューと同じ WM_SYSCOMMAND を PostMessage で送る。窓は前面に出さない):
     音質優先 → 速度優先 → 鳴らさない(サーバーの取り込みが止まる)→ 鳴らす
WAV は、届いた音を輪に入れるとき(復号の後、再生の前)に書く。再生の側の途切れはログの「途切れ」で見る。
"""
import argparse, ctypes, os, re, subprocess, sys, time, wave
from ctypes import wintypes as W
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, '..', 'server', 'tools'))
import iivcheck  # noqa: E402

TEST = os.path.join(ROOT, 'build', 'test')
RATE = 48000


def reference(n):
    """サーバーの -testaudio と同じ音(位置 0 から n サンプル)"""
    i = np.arange(n, dtype=np.float64)
    t = (i % 96000) / 48000.0
    left = np.trunc(0.3 * 32767 * np.sin(2 * np.pi * (200 * t + (8000 - 200) * t * t / 4)))
    right = np.trunc(0.3 * 32767 * np.sin(2 * np.pi * 1000 * i / 48000.0))
    return left, right


REF_L, REF_R = reference(96000 * 2)     # 2 周期(位置を mod 96000 で探す)


def find_offset(seg):
    """seg(左)がスイープの何サンプル目から始まるか(0〜95999)。円状の相関で探す"""
    n = 96000
    a = np.zeros(n)
    m = min(len(seg), n)
    a[:m] = seg[:m]
    c = np.fft.irfft(np.fft.rfft(REF_L[:n]) * np.conj(np.fft.rfft(a)), n)
    return int(np.argmax(c))


def analyze(path, exact):
    with wave.open(path, 'rb') as w:
        if w.getframerate() != RATE or w.getnchannels() != 2:
            return dict(err=f'形が違う {w.getframerate()} {w.getnchannels()}')
        raw = w.readframes(w.getnframes())
    x = np.frombuffer(raw, dtype='<i2').reshape(-1, 2).astype(np.float64)
    n = len(x)
    if n < RATE:
        return dict(err=f'短すぎる {n} サンプル')
    # 無音で始まる部分(AAC の先頭など)は飛ばす
    start = int(np.argmax(np.abs(x[:, 1]) > 100))
    pos, jumps, sig, err, bad = start, 0, 0.0, 0.0, 0
    block = 4800
    off = find_offset(x[pos:pos + 96000, 0])
    while pos + block <= n:
        seg = x[pos:pos + block]
        ref = np.stack([REF_L[off:off + block], REF_R[off:off + block]], axis=1) if off + block <= len(REF_L) else None
        if ref is None:
            off %= 96000
            continue
        e = seg - ref
        bsig, berr = float((ref ** 2).sum()), float((e ** 2).sum())
        if berr > bsig * (1e-3 if not exact else 1e-6):
            # ずれた(欠けた): 位置を探し直す
            noff = find_offset(x[pos:pos + 96000, 0])
            if noff != off:
                jumps += 1
                off = noff
                continue
            bad += 1
        sig += bsig
        err += berr
        if exact:
            bad += int((np.abs(e) > 1.5).any())
        pos += block
        off = (off + block) % 96000
    snr = 10 * np.log10(sig / err) if err > 0 else float('inf')
    return dict(samples=n, seconds=n / RATE, start=start, jumps=jumps, bad_blocks=bad, snr=snr)


def run_client(audio, seconds, tag):
    cini = os.path.join(TEST, f'ac-{tag}.ini')
    log = os.path.join(TEST, f'ac-{tag}.log')
    wav = os.path.join(TEST, f'ac-{tag}.wav')
    for f in (log, wav):
        if os.path.exists(f):
            os.remove(f)
    with open(cini, 'w', encoding='utf-8') as f:
        f.write('[client]\nquality=auto\nrender=gpu\n[general]\nlog=1\n')
    si = subprocess.STARTUPINFO()
    si.dwFlags = 1
    si.wShowWindow = 4
    p = subprocess.Popen([os.path.join(ROOT, 'iiv-client.exe'), '-ini', cini, '127.0.0.1:5999', '-password', 'bench',
                          '-exitafter', str(int(seconds * 60)), '-log', '-audio', audio, '-audiomute', '-audiodump', wav],
                         startupinfo=si)
    try:
        p.wait(seconds + 60)
    except subprocess.TimeoutExpired:
        p.kill()
    clog = open(log, encoding='utf-8').read() if os.path.exists(log) else ''
    return clog, wav


def last_stats(clog):
    m = re.findall(r'音: パケット (\d+)、抜け (\d+)、途切れ (\d+)、捨てた (\d+)、溜め ([\d.]+)ms、届くまで ([\d.]+)ms、速さ ([-+\d.]+)%', clog)
    return m[-1] if m else None


def fps(clog):
    m = re.search(r'更新 \d+ 回\(([\d.]+) 回/秒\)、受信 \d+ バイト\(([\d.]+) Mbps\)', clog)
    return (float(m[1]), float(m[2])) if m else (0, 0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--seconds', type=float, default=15)
    a = ap.parse_args()
    os.makedirs(TEST, exist_ok=True)
    ok = True

    def check(name, cond, detail=''):
        nonlocal ok
        ok &= bool(cond)
        print(f'  {name:<44} {"OK" if cond else "NG"}  {detail}')

    for audio, exact, tag in (('quality', True, 'pcm'), ('speed', False, 'aac')):
        print(f'[{"音質優先(PCM)" if exact else "速度優先(AAC)"}: {a.seconds:.0f} 秒]')
        iivcheck.start_server(['-testsrc', 'video', '-testfps', '0', '-testaudio'], password='bench', extra='audio=1\n')
        clog, wav = run_client(audio, a.seconds, tag)
        iivcheck.stop_server()
        slog = iivcheck.server_log()
        st = last_stats(clog)
        r = analyze(wav, exact) if os.path.exists(wav) else dict(err='WAV が無い')
        f, mbps = fps(clog)
        check('映像', f >= 55, f'{f:.1f} フレーム/秒 {mbps:.1f} Mbps')
        if 'err' in r:
            check('受け取った音', False, r['err'])
            continue
        if exact:
            check('サーバーの音と同じ(±1)', r['bad_blocks'] == 0 and r['jumps'] == 0,
                  f'{r["seconds"]:.1f} 秒、ずれ {r["jumps"]} 回、合わない 100ms {r["bad_blocks"]} 個、SNR {r["snr"]:.1f}dB')
        else:
            check('SNR 30dB 以上、欠けなし', r['snr'] >= 30 and r['jumps'] == 0,
                  f'{r["seconds"]:.1f} 秒、ずれ {r["jumps"]} 回、SNR {r["snr"]:.1f}dB、先頭の無音 {r["start"] / 48:.0f}ms')
        if st:
            pk, lost, under, drops, buf, arrive, speed = st
            check('抜け・途切れ・捨てた', lost == '0' and under == '0' and drops == '0',
                  f'パケット {pk}、抜け {lost}、途切れ {under}、捨てた {drops}、溜め {buf}ms、届くまで {arrive}ms、速さ {speed}%')
        else:
            check('クライアントの音の集計', False, clog[-600:])
        sent = re.findall(r'音 (\d+)\(捨てた (\d+)\)', slog)
        if sent:
            check('サーバーで捨てた音', sent[-1][1] == '0', f'送った {sent[-1][0]}、捨てた {sent[-1][1]}')

    print('[サーバーの「音を鳴らす」が切られている]')
    iivcheck.start_server(['-testsrc', 'video', '-testfps', '0', '-testaudio'], password='bench', extra='audio=0\n')
    clog, wav = run_client('quality', 4, 'off-server')
    iivcheck.stop_server()
    slog = iivcheck.server_log()
    check('クライアントに「切られている」が届く', '切られている' in clog)
    check('サーバーは取り込みを始めない', 'スレッドを始めた' not in slog)
    check('音を鳴らさない(WAV なし)', not os.path.exists(wav) and '鳴らし始めた' not in clog)

    print('[クライアントが「音を鳴らす」にしていない]')
    iivcheck.start_server(['-testsrc', 'video', '-testfps', '0', '-testaudio'], password='bench', extra='audio=1\n')
    clog, wav = run_client('off', 4, 'off-client')
    iivcheck.stop_server()
    slog = iivcheck.server_log()
    check('サーバーは音を求められない', '音を求められた' not in slog)
    check('サーバーは取り込みを始めない', 'スレッドを始めた' not in slog)
    f, mbps = fps(clog)
    check('映像', f >= 55, f'{f:.1f} フレーム/秒 {mbps:.1f} Mbps')

    print('[つないだまま切り替える]')
    # view.c の IDM_*(0x100 から順に。IDM_AUDIO = 0x110、IDM_AUDIO_QUALITY = 0x111、IDM_AUDIO_SPEED = 0x112)
    IDM_AUDIO, IDM_AUDIO_SPEED = 0x110, 0x112
    u = ctypes.windll.user32
    iivcheck.start_server(['-testsrc', 'video', '-testfps', '0', '-testaudio'], password='bench', extra='audio=1\n')
    cini = os.path.join(TEST, 'ac-switch.ini')
    log = os.path.join(TEST, 'ac-switch.log')
    if os.path.exists(log):
        os.remove(log)
    with open(cini, 'w', encoding='utf-8') as f:
        f.write('[client]\nquality=auto\nrender=gpu\n[general]\nlog=1\n')
    si = subprocess.STARTUPINFO()
    si.dwFlags = 1
    si.wShowWindow = 4
    p = subprocess.Popen([os.path.join(ROOT, 'iiv-client.exe'), '-ini', cini, '127.0.0.1:5999', '-password', 'bench',
                          '-log', '-audio', 'quality', '-audiomute'], startupinfo=si)
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
        u.PostMessageW(h, 0x112, IDM_AUDIO_SPEED, 0)    # 速度優先へ
        time.sleep(3)
        u.PostMessageW(h, 0x112, IDM_AUDIO, 0)          # 鳴らさない
        time.sleep(4)                                   # サーバーの取り込みは 2 秒で止まる
        mid = iivcheck.server_log()
        u.PostMessageW(h, 0x112, IDM_AUDIO, 0)          # また鳴らす(速度優先のまま)
        time.sleep(3)
        u.PostMessageW(h, 0x10, 0, 0)
        p.wait(timeout=10)
    finally:
        if p.poll() is None:
            p.kill()
        iivcheck.stop_server()
    slog = iivcheck.server_log()
    clog = open(log, encoding='utf-8').read() if os.path.exists(log) else ''
    req = re.findall(r'音を求められた\((\S+?)\)', slog)
    check('サーバーが受けた求め', req == ['音質優先', '速度優先', '要らない', '速度優先'], ' → '.join(req))
    check('鳴らさないにしたら、サーバーの取り込みが止まる', 'スレッドを終えた' in mid)
    check('また鳴らすと、取り込みが始まる', slog.count('スレッドを始めた') == 2)
    forms = re.findall(r'サーバーの形 (\S+?)、', clog)
    check('クライアントが受けた形', forms == ['PCM', 'AAC', 'AAC'], ' → '.join(forms))
    st = last_stats(clog)
    check('途切れ・捨てた', st and st[2] == '0' and st[3] == '0', f'最後の集計: 途切れ {st[2]}、捨てた {st[3]}' if st else '集計なし')

    print('ALL OK' if ok else 'NG あり')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
