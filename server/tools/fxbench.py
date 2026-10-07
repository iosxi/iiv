"""ファイル転送(貼り付け)の速さを、遅い回線をまねて測る。

    python tools/fxbench.py [--files 1000] [--rtt 40] [--mbps 20] [--video on|off|both]
    python tools/fxbench.py --explorer [--old build/v7] [--files 1000 | --big 10] [--rtt 40] [--mbps 20]

- テスト用のフォルダ(build/test/fxdata/many に細かいファイル --files 個)を作り、クリップボードに
  ファイルとして置く(元のクリップボードの文字は最後に戻す)。サーバーは fxoffer=1 なので、
  つないだときにその一覧を送ってくる。
- サーバーとこの受け手の間に、遅れ(--rtt の半分ずつ)と帯域(--mbps、両方向)を絞る中継を挟む。
  中継の溜まりは 256KB まで(それを超えると読まない = 送り手の TCP が待つ)。
- 受け手はエクスプローラーの貼り付けと同じ読み方をする: ファイルを 1 つずつ順に、
  512KB ずつ、6 つ先まで FX_READ で頼む(filexfer.c の VStream と同じ)。
- 映像あり = -testsrc video(毎フレーム全面が変わる)、なし = -testsrc static。受け手は映像に返事(ACK)を返す。

--explorer: 受け手を本物の iiv-client にし、クライアントがクリップボードに置いた一覧を tools/fxpaste.c
(エクスプローラーの貼り付けと同じく、貼り付け先フォルダの IDropTarget へ落とす)で貼り付けて、
届くまでの時間と中身の一致を見る。--big N は、細かいファイルの代わりに 2MB のファイルを N 個(乱数とテキストを交互。大きいファイルの圧縮と、効かないときの経路を通す)。
--old に古い版の exe の置き場所を渡すと、それとも比べる(例: git show v7:server/iiv-server.exe などで取り出す)。
"""
import argparse, ctypes, os, random, socket, struct, subprocess, sys, threading, time, asyncio

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import iivcheck as t

FX_FILES, FX_READ, FX_DATA = 2, 3, 4
CHUNK, WINDOW = 512 * 1024, 6
PROXY_PORT = 5998
FXPASTE = os.path.join(t.ROOT, 'build', 'mf', 'fxpaste.exe')   # tools/build-tools.bat で作る
QUEUE_LIMIT = 256 * 1024

WORDS = ('iiv server client frame video audio file copy paste window screen mouse key network '
         'latency bandwidth packet stream buffer thread queue chunk folder explorer windows').split()


def make_files(n, big=False):
    d = os.path.join(t.TEST, 'fxdata', 'big' if big else 'many')
    os.makedirs(d, exist_ok=True)
    have = sorted(os.listdir(d))
    if len(have) != n:
        for f in have:
            os.remove(os.path.join(d, f))
        rnd = random.Random(1)
        for i in range(n):
            if big:                     # 乱数(圧縮が効かない)とテキスト(よく縮む)を交互に。2MB ずつ
                with open(os.path.join(d, f'b{i:03d}.bin'), 'wb') as f:
                    if i % 2 == 0:
                        f.write(rnd.randbytes(2 << 20))
                    else:
                        words = [rnd.choice(WORDS) if rnd.random() < 0.8 else str(rnd.randint(0, 99999)) for _ in range(400000)]
                        f.write(' '.join(words).encode()[:2 << 20].ljust(2 << 20, b'.'))
                continue
            size = rnd.randint(1024, 8192)
            text = []
            while sum(len(w) + 1 for w in text) < size:
                text.append(rnd.choice(WORDS) if rnd.random() < 0.8 else str(rnd.randint(0, 99999)))
            with open(os.path.join(d, f'f{i:05d}.txt'), 'w', newline='\n') as f:
                f.write(' '.join(text)[:size])
    total = sum(os.path.getsize(os.path.join(d, f)) for f in os.listdir(d))
    return d, total


def ps(cmd):
    return subprocess.run(['powershell', '-NoProfile', '-Command', cmd], capture_output=True, text=True,
                          encoding='utf-8', errors='replace').stdout


# ------------------------------------------------------------------
#  遅い回線をまねる中継
# ------------------------------------------------------------------

class Link:
    """片方向。読んだ塊を帯域ぶんの時間で送り出し、遅れを足して書く"""
    def __init__(self, mbps, delay):
        self.bps = mbps * 1e6 if mbps else 0
        self.delay = delay
        self.free = 0.0
        self.queued = 0

    async def run(self, reader, writer):
        q = asyncio.Queue()
        cond = asyncio.Condition()

        async def pump_in():
            while True:
                d = await reader.read(16384)
                if not d:
                    await q.put(None)
                    return
                async with cond:
                    await cond.wait_for(lambda: self.queued < QUEUE_LIMIT)
                    self.queued += len(d)
                now = time.perf_counter()
                start = max(now, self.free)
                self.free = start + (len(d) * 8 / self.bps if self.bps else 0)
                await q.put((self.free + self.delay, d))

        async def pump_out():
            while True:
                it = await q.get()
                if it is None:
                    writer.close()
                    return
                at, d = it
                w = at - time.perf_counter()
                if w > 0:
                    await asyncio.sleep(w)
                writer.write(d)
                await writer.drain()
                async with cond:
                    self.queued -= len(d)
                    cond.notify_all()

        await asyncio.gather(pump_in(), pump_out(), return_exceptions=True)


def start_proxy(mbps, rtt_ms):
    ready = threading.Event()

    async def handle(cr, cw):
        sr, sw = await asyncio.open_connection('127.0.0.1', t.PORT)
        for w in (cw, sw):
            w.get_extra_info('socket').setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        await asyncio.gather(Link(mbps, rtt_ms / 2000).run(cr, sw), Link(mbps, rtt_ms / 2000).run(sr, cw),
                             return_exceptions=True)

    async def main():
        srv = await asyncio.start_server(handle, '127.0.0.1', PROXY_PORT)
        ready.set()
        async with srv:
            await srv.serve_forever()

    th = threading.Thread(target=lambda: asyncio.run(main()), daemon=True)
    th.start()
    ready.wait(5)


# ------------------------------------------------------------------
#  受け手
# ------------------------------------------------------------------

class FastConn(t.Conn):
    """映像を読む手間で FX_DATA が遅れないよう、読み込みを bytearray とずらし位置で"""
    def read(self, n):
        if not isinstance(self.buf, bytearray):
            self.buf, self.pos = bytearray(self.buf), 0
        while len(self.buf) - self.pos < n:
            if self.pos:
                del self.buf[:self.pos]
                self.pos = 0
            d = self.s.recv(1 << 20)
            if not d:
                raise EOFError('切れた')
            self.buf += d
            self.bytes += len(d)
        r = bytes(self.buf[self.pos:self.pos + n])
        self.pos += n
        return r


class Receiver:
    def __init__(self, port):
        self.c = FastConn(port=port, password='fx', files=True)
        self.lock = threading.Lock()
        self.offer = None
        self.slots = {}
        self.video_bytes = 0
        self.fx_bytes = 0
        self.stop = False
        threading.Thread(target=self.loop, daemon=True).start()

    def send(self, typ, body):
        with self.lock:
            self.c.send(typ, body)

    def loop(self):
        try:
            while not self.stop:
                typ, b = self.c.msg()
                if typ == t.S_FX:
                    self.on_fx(b[0], b[1:])
                    continue
                with self.lock:
                    v = self.c.handle(typ, b)
                if v:
                    self.video_bytes += len(v[4])
                    self.send(t.C_ACK, struct.pack('<II', v[0], 0))
        except (EOFError, OSError):
            pass

    def on_fx(self, sub, b):
        if sub == FX_FILES:
            oid, n = struct.unpack('<QI', b[:12])
            p, ents = 12, []
            for _ in range(n):
                fl, attr, size, mtime, rl = struct.unpack('<BIQQH', b[p:p + 23])
                p += 23
                name = b[p:p + rl * 2].decode('utf-16-le')
                p += rl * 2
                ents.append((name, bool(fl & 1), size))
            self.offer = (oid, ents)
        elif sub == FX_DATA:
            rid, st = struct.unpack('<II', b[:8])
            self.fx_bytes += len(b) - 8
            ev = self.slots.get(rid)
            if ev:
                ev[1] = (st, b[8:])
                ev[0].set()

    def read_file(self, oid, index, size, rid):
        """エクスプローラーの読み方: 512KB ずつ、6 つ先まで"""
        pos = req = 0
        pend = []
        while pos < size:
            while len(pend) < WINDOW and req < size:
                n = min(CHUNK, size - req)
                rid[0] += 1
                ev = [threading.Event(), None]
                self.slots[rid[0]] = ev
                self.send(t.C_FX, bytes([FX_READ]) + struct.pack('<IQIQI', rid[0], oid, index, req, n))
                pend.append((rid[0], n))
                req += n
            r, n = pend.pop(0)
            ev = self.slots[r]
            if not ev[0].wait(30):
                raise SystemExit(f'{index} 番目の中身が来ない')
            del self.slots[r]
            st, d = ev[1]
            if st or len(d) != n:
                raise SystemExit(f'{index} 番目を読めない status={st} len={len(d)}')
            pos += n


def run(video, files_dir, nfiles, total, mbps, rtt):
    t.start_server(['-testsrc', 'video' if video else 'static'], password='fx', extra='fxoffer=1\n')
    try:
        r = Receiver(PROXY_PORT)
        for _ in range(100):
            if r.offer:
                break
            time.sleep(0.1)
        if not r.offer:
            raise SystemExit('一覧が来ない(クリップボードにファイルが無い?)')
        oid, ents = r.offer
        files = [(i, e) for i, e in enumerate(ents) if not e[1]]
        time.sleep(2)                       # 映像が流れ始めるのを待つ
        v0, rid = r.video_bytes, [0]
        t0 = time.perf_counter()
        for i, e in files:
            r.read_file(oid, i, e[2], rid)
        dt = time.perf_counter() - t0
        vmb = (r.video_bytes - v0) * 8 / dt / 1e6
        r.stop = True
        r.c.s.close()
        return dict(files=len(files), dt=dt, per_file_ms=dt * 1000 / len(files), mbps=total * 8 / dt / 1e6, video_mbps=vmb)
    finally:
        t.stop_server()


# ------------------------------------------------------------------
#  本物: iiv-client が受け取った一覧を、エクスプローラー(シェルの「貼り付け」)で貼り付ける
# ------------------------------------------------------------------

def clip_has_virtual_files():
    u = ctypes.windll.user32
    fmt = u.RegisterClipboardFormatW('FileGroupDescriptorW')
    return bool(u.IsClipboardFormatAvailable(fmt))


def same_tree(src, dst):
    for root, _, files in os.walk(src):
        for f in files:
            a = os.path.join(root, f)
            b = os.path.join(dst, os.path.relpath(a, src))
            if not os.path.exists(b) or open(a, 'rb').read() != open(b, 'rb').read():
                return False, os.path.relpath(a, src)
    return True, None


def run_explorer(label, server_exe, client_exe, src, total, reverse=False):
    """reverse = クライアントでコピーして、サーバー(が置いた一覧)を貼り付ける"""
    t.EXE = server_exe
    t.start_server(['-testsrc', 'video'], password='fx', extra='' if reverse else 'fxoffer=1\n')
    ini = os.path.join(t.TEST, 'fxb-client.ini')
    with open(ini, 'w', encoding='utf-8') as f:
        f.write('[client]\nrender=gdi\nfit=1\n[general]\nlog=1\n')
    dest = os.path.join(t.TEST, 'fxdata', 'dest')
    if os.path.exists(dest):
        subprocess.run(['cmd', '/c', 'rmdir', '/s', '/q', dest])
    os.makedirs(dest)
    want = len(os.listdir(src))
    si = subprocess.STARTUPINFO()
    si.dwFlags = 1
    si.wShowWindow = 7                  # 最小化・前面に出さない
    cl = subprocess.Popen([client_exe, '-ini', ini, f'127.0.0.1:{PROXY_PORT}', '-password', 'fx']
                          + (['-fxoffer'] if reverse else []), startupinfo=si)
    paste = None
    try:
        for _ in range(200):
            if clip_has_virtual_files():
                break
            time.sleep(0.05)
        else:
            raise SystemExit('受け取った側が一覧をクリップボードに置かない')
        time.sleep(2)                   # 映像が流れ始めるのを待つ
        t0 = time.perf_counter()
        paste = subprocess.Popen([FXPASTE, dest], stdout=subprocess.DEVNULL)
        out = os.path.join(dest, os.path.basename(src))
        while True:
            have = os.listdir(out) if os.path.isdir(out) else []
            if len(have) == want and sum(os.path.getsize(os.path.join(out, f)) for f in have) == total:
                break
            if time.perf_counter() - t0 > 590:
                raise SystemExit(f'終わらない({len(have)} / {want} 個)')
            time.sleep(0.02)
        dt = time.perf_counter() - t0
        ok, bad = same_tree(src, out)
        print(f'  {label}: {dt:.1f} 秒  1 ファイル {dt * 1000 / want:.1f} ms  中身 {total * 8 / dt / 1e6:.2f} Mbps  '
              f'{"中身は一致" if ok else "中身が違う: " + bad}', flush=True)
        return dt
    finally:
        if paste:
            paste.kill()
        cl.kill()
        t.stop_server()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--files', type=int, default=1000)
    ap.add_argument('--big', type=int, default=0, help='2MB のファイル(乱数とテキストを交互)を N 個(--explorer のとき)')
    ap.add_argument('--rtt', type=float, default=40)
    ap.add_argument('--mbps', type=float, default=20)
    ap.add_argument('--video', choices=['on', 'off', 'both'], default='both')
    ap.add_argument('--explorer', action='store_true', help='本物の iiv-client とエクスプローラーの貼り付けで測る')
    ap.add_argument('--old', help='比べる古い版の exe の置き場所(iiv-server.exe と iiv-client.exe)')
    a = ap.parse_args()

    ctypes.windll.winmm.timeBeginPeriod(1)
    d, total = make_files(a.big or a.files, big=a.big > 0)
    saved = ps('Get-Clipboard -Raw')
    start_proxy(a.mbps, a.rtt)
    print(f'{"2MB のファイル" if a.big else "細かいファイル"} {a.big or a.files} 個(計 {total / 1024:.0f} KB)、回線 RTT {a.rtt:.0f}ms・{a.mbps:.0f}Mbps(0 = 絞らない)')
    try:
        if a.explorer:
            client = os.path.join(os.path.dirname(t.ROOT), 'client', 'iiv-client.exe')
            server = os.path.join(t.ROOT, 'iiv-server.exe')
            runs = []
            if a.old:
                old_s, old_c = os.path.join(a.old, 'iiv-server.exe'), os.path.join(a.old, 'iiv-client.exe')
                runs.append(('古い版どうし', old_s, old_c, False))
            runs.append(('この版 サーバー → クライアント', server, client, False))
            runs.append(('この版 クライアント → サーバー', server, client, True))
            if a.old:
                runs.append(('この版のサーバー × 古い版のクライアント', server, old_c, False))
                runs.append(('古い版のサーバー × この版のクライアント', old_s, client, False))
            for label, sv, cl, rev in runs:
                ps(f"Set-Clipboard -Path '{d}'")
                run_explorer(label, sv, cl, d, total, rev)
            return
        ps(f"Set-Clipboard -Path '{d}'")
        for v in ([True, False] if a.video == 'both' else [a.video == 'on']):
            m = run(v, d, a.files, total, a.mbps, a.rtt)
            print(f"  映像{'あり' if v else 'なし'}: {m['dt']:.1f} 秒  1 ファイル {m['per_file_ms']:.1f} ms  "
                  f"中身 {m['mbps']:.2f} Mbps  映像 {m['video_mbps']:.1f} Mbps", flush=True)
    finally:
        if saved.strip():
            p = os.path.join(t.TEST, 'clip-saved.txt')
            with open(p, 'w', encoding='utf-8') as f:
                f.write(saved.rstrip('\n'))
            ps(f"Get-Content -Raw -Encoding utf8 '{p}' | Set-Clipboard")
            os.remove(p)


if __name__ == '__main__':
    main()
