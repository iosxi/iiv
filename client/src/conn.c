/* ==================================================================
 * conn.c - 通信のスレッド
 *
 *  接続、iiv の手順(iivproto.h)、受信と復号、送信。
 *
 *  映像はサーバーが押し出してくる(こちらから求めない)。H.264 のフレームを受けたら
 *  このスレッドで復号して画面の写しに書き、窓へ知らせる。窓が表示し終えたら
 *  conn_frame_shown で「表示した」と返す。サーバーは返事の無いフレームが 2 つを超えると
 *  待つので、こちらが遅ければ、サーバー側で変化がまとまる(遅れが溜まらない)。
 *
 *  音(「音を鳴らす」)は、つないだ後に IIV_C_AUDIO で求める。届いた音は audio.c の待ち行列へ
 *  写すだけで、復号と再生は audio.c のスレッドがする(このスレッドの映像の復号を待たせない)。
 *
 *  カーソルはサーバーから形と位置を受け取り、こちらで描く(動かしても往復を待たない)。
 *  送信(キー・マウス・クリップボード・返事)は画面のスレッドからも呼ばれるので
 *  g_sendCs で 1 つずつにする。
 * ================================================================== */

#include "iivc.h"
#include <objbase.h>
#include <bcrypt.h>
#include <stdarg.h>

#pragma comment(lib, "bcrypt.lib")

Remote g_rm;

static SOCKET           g_sock = INVALID_SOCKET;
static HANDLE           g_thread;
static HWND             g_notify;
static ConnParams       g_p;
static volatile LONG    g_stop;
static CRITICAL_SECTION g_sendCs;
static BOOL             g_inited;
static WCHAR            g_err[512];
static BOOL             g_authFailed, g_needPw;
static volatile LONG    g_active;
static volatile LONG    g_connGen;     /* 接続ごとに増やす(ファイルの受け渡しで、前の接続宛てのものを断る) */
static volatile LONG    g_fxOK;        /* 相手もファイルを受け渡せる */
static volatile LONG    g_fxBatch;     /* 相手もまとめ読みと圧縮が分かる */
static LONG64           g_qpf;
static BOOL             g_vdecOk;
static volatile LONG    g_decReset;    /* 描画の方式が変わった: 復号器を作り直す */
static IivVideoConfig   g_vc;          /* 今の映像の形 */

/* 画質(Q_*)ごとに求めるビットレート。0 = サーバーに任せる */
static const unsigned k_kbps[Q_COUNT] = { 0, 60000, 10000, 3000 };

/* 受信バッファ */
static BYTE *g_rb;
static int   g_rbCap, g_rbPos, g_rbLen;

static void set_error(const WCHAR *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(g_err, ARRAYSIZE(g_err) - 1, fmt, ap);
    va_end(ap);
    g_err[ARRAYSIZE(g_err) - 1] = 0;
}

static LONG64 qpc_now(void)
{
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return q.QuadPart;
}

/* ------------------------------------------------------------------ */
/*  受信                                                                */
/* ------------------------------------------------------------------ */

static BOOL fill(int need)
{
    if (g_rbLen - g_rbPos >= need) return TRUE;
    if (g_rbPos) {
        memmove(g_rb, g_rb + g_rbPos, (size_t)(g_rbLen - g_rbPos));
        g_rbLen -= g_rbPos;
        g_rbPos = 0;
    }
    if (need > g_rbCap) {
        int cap = need + (1 << 20);
        BYTE *p = (BYTE *)realloc(g_rb, (size_t)cap);
        if (!p) return FALSE;
        g_rb = p;
        g_rbCap = cap;
    }
    while (g_rbLen < need) {
        int r = recv(g_sock, (char *)g_rb + g_rbLen, g_rbCap - g_rbLen, 0);
        if (r <= 0 || g_stop) return FALSE;
        g_rbLen += r;
        InterlockedAdd64(&g_rm.bytes, r);
    }
    return TRUE;
}

static BOOL rd(void *buf, int n)
{
    if (n <= 0) return TRUE;
    if (!fill(n)) return FALSE;
    memcpy(buf, g_rb + g_rbPos, (size_t)n);
    g_rbPos += n;
    return TRUE;
}

static BYTE *rd_ptr(int n)
{
    BYTE *p;
    if (!fill(n)) return NULL;
    p = g_rb + g_rbPos;
    g_rbPos += n;
    return p;
}

/* 大きなメッセージのために広げた受信バッファを、使い終わったら縮める */
static void rd_trim(void)
{
    int left = g_rbLen - g_rbPos;
    if (g_rbCap <= (8 << 20) || left > (1 << 20)) return;
    memmove(g_rb, g_rb + g_rbPos, (size_t)left);
    g_rbLen = left;
    g_rbPos = 0;
    {
        BYTE *p = (BYTE *)realloc(g_rb, 2 << 20);
        if (p) { g_rb = p; g_rbCap = 2 << 20; }
    }
}

/* ------------------------------------------------------------------ */
/*  送信                                                                */
/* ------------------------------------------------------------------ */

static BOOL send_all(const void *data, int len)
{
    const char *p = (const char *)data;
    while (len > 0) {
        int r = send(g_sock, p, len, 0);
        if (r <= 0) return FALSE;
        p += r;
        len -= r;
    }
    return TRUE;
}

static BOOL send_msg(int type, const void *a, int na, const void *b, int nb)
{
    BYTE h[5];
    BOOL ok;
    unsigned len = 1u + (unsigned)na + (unsigned)nb;
    if (!g_active) return FALSE;
    memcpy(h, &len, 4);
    h[4] = (BYTE)type;
    EnterCriticalSection(&g_sendCs);
    ok = g_sock != INVALID_SOCKET && send_all(h, 5) && (!na || send_all(a, na)) && (!nb || send_all(b, nb));
    LeaveCriticalSection(&g_sendCs);
    return ok;
}

/* 映像の座標 → 相手の取り込んだ範囲の座標 */
static void to_desk(int *x, int *y)
{
    if (g_rm.w > 0 && g_rm.deskW > 0 && g_rm.w != g_rm.deskW) *x = (int)((*x + 0.5) * g_rm.deskW / g_rm.w);
    if (g_rm.h > 0 && g_rm.deskH > 0 && g_rm.h != g_rm.deskH) *y = (int)((*y + 0.5) * g_rm.deskH / g_rm.h);
}

void conn_send_pointer(int buttons, int x, int y)
{
    IivMouse m;
    if (g_p.viewOnly) return;
    to_desk(&x, &y);
    ZeroMemory(&m, sizeof(m));
    m.x = x;
    m.y = y;
    m.buttons = (unsigned char)buttons;
    send_msg(IIV_C_MOUSE, &m, sizeof(m), NULL, 0);
}

void conn_send_wheel(int buttons, int x, int y, int delta, BOOL horizontal)
{
    IivMouse m;
    if (g_p.viewOnly || !delta) return;
    to_desk(&x, &y);
    ZeroMemory(&m, sizeof(m));
    m.x = x;
    m.y = y;
    m.buttons = (unsigned char)buttons;
    if (horizontal) m.hwheel = (short)delta; else m.wheel = (short)delta;
    send_msg(IIV_C_MOUSE, &m, sizeof(m), NULL, 0);
}

void conn_send_key(BOOL down, UINT vk, UINT scan, BOOL ext)
{
    IivKey k;
    if (g_p.viewOnly) return;
    k.scan = (unsigned short)((scan & 0xFF) | (ext ? 0x100 : 0));
    k.vk = (unsigned short)vk;
    k.down = (unsigned char)(down != 0);
    send_msg(IIV_C_KEY, &k, sizeof(k), NULL, 0);
}

void conn_send_sas(void)
{
    if (!g_p.viewOnly) send_msg(IIV_C_SAS, NULL, 0, NULL, 0);
}

void conn_request_keyframe(void)
{
    send_msg(IIV_C_KEYFRAME, NULL, 0, NULL, 0);
}

void conn_set_quality(int q)
{
    IivSettings st;
    if (q < 0 || q >= Q_COUNT) q = Q_AUTO;
    g_p.quality = q;
    st.kbps = k_kbps[q];
    send_msg(IIV_C_SETTINGS, &st, sizeof(st), NULL, 0);
}

int audio_codec_from_cfg(void)
{
    if (!g_cfg.audio) return IIV_AUDIO_OFF;
    return g_cfg.audioSpeed ? IIV_AUDIO_AAC : IIV_AUDIO_PCM;
}

void conn_set_audio(int codec)
{
    IivAudioRequest ar;
    g_p.audio = codec;
    ar.codec = (unsigned char)codec;
    if (!codec) {
        audio_stop();
        audio_reset_status();
    }
    send_msg(IIV_C_AUDIO, &ar, sizeof(ar), NULL, 0);
}

void conn_frame_shown(UINT32 frame, LONG64 recvQpc)
{
    IivAck a;
    LONG64 now = qpc_now();
    a.frame = frame;
    a.decodeUs = recvQpc && g_qpf ? (unsigned)((now - recvQpc) * 1000000 / g_qpf) : 0;
    send_msg(IIV_C_ACK, &a, sizeof(a), NULL, 0);
}

void conn_send_clipboard(const char *utf8, int len)
{
    if (g_p.viewOnly || !g_active) return;
    send_msg(IIV_C_CLIPBOARD, utf8, len, NULL, 0);
}

/* ------------------------------------------------------------------ */
/*  ファイルのコピー＆貼り付け(filexfer.c)                              */
/* ------------------------------------------------------------------ */

static BOOL fx_usable(void) { return g_fxOK && !g_p.viewOnly; }

BOOL fx_host_send(int conn, int sub, const BYTE *p, int n)
{
    BYTE s = (BYTE)sub;
    if (conn != g_connGen || !g_active || !g_fxOK) return FALSE;
    return send_msg(IIV_C_FX, &s, 1, p, n);
}

BOOL fx_host_batch_ok(int conn) { return conn == g_connGen && g_active && g_fxOK && g_fxBatch; }

/* クライアントは利用者の権限で動いているので、なりすまさない */
HANDLE fx_host_user_token(void) { return NULL; }

void conn_send_files(HDROP hd)
{
    int   id = g_connGen, len = 0;
    BYTE *out = NULL;
    if (!g_active || !fx_usable()) return;
    if (!fx_make_offer(&id, 1, hd, &out, &len)) return;
    fx_host_send(id, FX_FILES, out, len);
    HeapFree(GetProcessHeap(), 0, out);
}

/* ------------------------------------------------------------------ */
/*  つなぐときの手順                                                    */
/* ------------------------------------------------------------------ */

static BOOL derive_proof(const char *pw, const BYTE *salt, unsigned iter, const BYTE *nonce, BYTE out[32])
{
    BCRYPT_ALG_HANDLE  alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    BYTE key[32], msg[40];
    BOOL ok = FALSE;
    ZeroMemory(out, 32);
    if (!iter) return TRUE;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG))) return FALSE;
    if (BCRYPT_SUCCESS(BCryptDeriveKeyPBKDF2(alg, (PUCHAR)pw, (ULONG)strlen(pw), (PUCHAR)salt, 16, iter, key, 32, 0))) {
        memcpy(msg, nonce, 32);
        memcpy(msg + 32, "iiv-auth", 8);
        if (BCRYPT_SUCCESS(BCryptCreateHash(alg, &h, NULL, 0, key, 32, 0)) &&
            BCRYPT_SUCCESS(BCryptHashData(h, msg, sizeof(msg), 0)) &&
            BCRYPT_SUCCESS(BCryptFinishHash(h, out, 32, 0))) ok = TRUE;
        if (h) BCryptDestroyHash(h);
    }
    SecureZeroMemory(key, sizeof(key));
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

static BOOL handshake(void)
{
    IivHello     hi;
    IivChallenge ch;
    IivWelcome   w;
    char         name[512];

    ZeroMemory(&hi, sizeof(hi));
    hi.magic = IIV_MAGIC;
    hi.version = IIV_VERSION;
    hi.codecs = 1u << IIV_CODEC_H264;
    hi.flags = IIV_HF_FILES | IIV_HF_FXBATCH;
    if (!send_all(&hi, sizeof(hi))) { set_error(L"サーバーへ送れませんでした。"); return FALSE; }
    if (!rd(&ch, sizeof(ch))) {
        set_error(L"サーバーが答えません。iiv-server ではないかもしれません(ポートを確かめてください)。");
        return FALSE;
    }
    if (ch.magic != IIV_MAGIC) { set_error(L"相手は iiv-server ではありません。"); return FALSE; }
    if (ch.auth == IIV_AUTH_PASSWORD) {
        BYTE proof[64];
        if (!g_p.password[0]) {
            g_needPw = TRUE;
            set_error(L"パスワードを入れてください。");
            return FALSE;
        }
        if (!derive_proof(g_p.password, ch.salt, ch.iterations, ch.nonce, proof) ||
            !derive_proof(g_p.password, ch.saltView, ch.iterationsView, ch.nonce, proof + 32)) {
            set_error(L"パスワードの計算に失敗しました。");
            return FALSE;
        }
        if (!send_all(proof, 64)) return FALSE;
    } else if (ch.auth != IIV_AUTH_NONE) {
        set_error(L"知らない認証の方式(%u)です。", ch.auth);
        return FALSE;
    }
    if (!rd(&w, sizeof(w)) || w.nameLen >= sizeof(name) || !rd(name, w.nameLen)) {
        set_error(L"サーバーが接続を閉じました。");
        return FALSE;
    }
    name[w.nameLen] = 0;
    if (w.result == IIV_BAD_PASSWORD) { g_authFailed = TRUE; set_error(L"パスワードが違います。"); return FALSE; }
    if (w.result == IIV_BLOCKED) { g_authFailed = TRUE; set_error(L"パスワードを何度も間違えたので、しばらく接続できません(1 分ほど待ってください)。"); return FALSE; }
    if (w.result == IIV_BAD_VERSION) { set_error(L"サーバーの版が違います。iiv-server と iiv-client を同じ版にそろえてください。"); return FALSE; }
    if (w.result != IIV_OK) { set_error(L"サーバーに断られました(%u)。", w.result); return FALSE; }
    if (w.flags & IIV_WF_VIEWONLY) g_p.viewOnly = TRUE;
    InterlockedExchange(&g_fxOK, (w.flags & IIV_WF_FILES) != 0);
    InterlockedExchange(&g_fxBatch, (w.flags & IIV_WF_FXBATCH) != 0);
    MultiByteToWideChar(CP_UTF8, 0, name, -1, g_rm.name, ARRAYSIZE(g_rm.name));
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  受け取ったメッセージ                                                */
/* ------------------------------------------------------------------ */

/* GPU で描くなら、窓のデバイスで(DXVA)。駄目なら CPU で */
static BOOL open_decoder(void)
{
    void *dev = NULL;
    BOOL  ok;
    if (!g_cfg.renderGdi) {
        DWORD_PTR r = 0;
        if (SendMessageTimeoutW(g_notify, WM_APP_D3D, 0, 0, SMTO_ABORTIFHUNG, 3000, &r)) dev = (void *)r;
    }
    ok = vdec_open(g_vc.videoW, g_vc.videoH, dev);
    if (dev) IUnknown_Release((IUnknown *)dev);
    return ok;
}

void conn_reset_decoder(void)
{
    InterlockedExchange(&g_decReset, 1);
}

static BOOL video_config(const IivVideoConfig *vc)
{
    BYTE *fb;
    int   w = vc->videoW, h = vc->videoH;
    if (vc->codec != IIV_CODEC_H264 || w <= 0 || h <= 0 || w > 8192 || h > 8192) {
        set_error(L"知らない映像の形です(%u %dx%d)。", vc->codec, w, h);
        return FALSE;
    }
    fb = (BYTE *)calloc((size_t)w * h + 8, 4);
    if (!fb) return FALSE;
    AcquireSRWLockExclusive(&g_rm.lock);
    free(g_rm.fb);
    g_rm.fb = fb;
    g_rm.w = w;
    g_rm.h = h;
    g_rm.deskW = vc->deskW;
    g_rm.deskH = vc->deskH;
    SetRectEmpty(&g_rm.dirty);
    ReleaseSRWLockExclusive(&g_rm.lock);
    g_vc = *vc;
    g_vdecOk = open_decoder();
    if (!g_vdecOk) { set_error(L"H.264 を復号できません(この PC に H.264 のデコーダがありません)。"); return FALSE; }
    log_printf(L"映像 %dx%d(相手の範囲 %ux%u)", w, h, vc->deskW, vc->deskH);
    PostMessageW(g_notify, WM_APP_RESIZE, 0, 0);
    return TRUE;
}

static BOOL video_frame(const BYTE *p, unsigned n)
{
    IivVideoHead vh;
    BOOL got = FALSE;
    LONG64 t0 = qpc_now(), t1;
    if (n < sizeof(vh)) return TRUE;
    memcpy(&vh, p, sizeof(vh));
    if (InterlockedExchange(&g_decReset, 0) && g_vc.videoW) {
        /* 作り直した復号器はキーフレームから。それまでのフレームは捨てる */
        g_vdecOk = open_decoder();
        if (g_vdecOk) { g_vdecOk = 2; conn_request_keyframe(); }
    }
    if (!g_vdecOk) return TRUE;
    if (g_vdecOk == 2) {
        if (!(vh.flags & IIV_VF_KEY)) return TRUE;
        g_vdecOk = TRUE;
    }
    if (!vdec_decode(p + sizeof(vh), (int)(n - sizeof(vh)), &got)) {
        log_printf(L"フレーム %u を復号できない。キーフレームを求める", vh.frame);
        conn_request_keyframe();
        return TRUE;
    }
    t1 = qpc_now();
    InterlockedAdd64(&g_rm.decodeTicks, t1 - t0);
    if (!got) return TRUE;
    InterlockedIncrement(&g_rm.updates);
    AcquireSRWLockExclusive(&g_rm.lock);
    g_rm.frameNo = vh.frame;
    g_rm.frameRecvQpc = t0;
    if (!g_rm.framePosted) {
        g_rm.framePosted = TRUE;
        PostMessageW(g_notify, WM_APP_FRAME, 0, 0);
    }
    ReleaseSRWLockExclusive(&g_rm.lock);
    return TRUE;
}

static void cursor_shape(const BYTE *p, unsigned n)
{
    IivCursorShape sh;
    int   w, h, mb, i, j;
    BYTE *bgra;
    const BYTE *pix, *mask;
    if (n < sizeof(sh)) return;
    memcpy(&sh, p, sizeof(sh));
    w = sh.w; h = sh.h;
    mb = (w + 7) / 8;
    if (w <= 0 || h <= 0 || w > 512 || h > 512 || n < sizeof(sh) + (unsigned)(w * h * 4 + mb * h)) return;
    pix = p + sizeof(sh);
    mask = pix + w * h * 4;
    bgra = (BYTE *)calloc((size_t)w * h, 4);
    if (!bgra) return;
    if (sh.visible)
        for (j = 0; j < h; j++)
            for (i = 0; i < w; i++) {
                BYTE *o = bgra + ((size_t)j * w + i) * 4;
                const BYTE *s = pix + ((size_t)j * w + i) * 4;
                o[0] = s[0]; o[1] = s[1]; o[2] = s[2];
                o[3] = (mask[j * mb + i / 8] >> (7 - (i & 7))) & 1 ? 255 : 0;
            }
    AcquireSRWLockExclusive(&g_rm.lock);
    free(g_rm.curPix);
    g_rm.curPix = bgra;
    g_rm.curW = w; g_rm.curH = h;
    g_rm.curHotX = sh.hotX; g_rm.curHotY = sh.hotY;
    g_rm.curVer++;
    g_rm.haveCursorEnc = TRUE;
    ReleaseSRWLockExclusive(&g_rm.lock);
    PostMessageW(g_notify, WM_APP_CURSOR, 0, 0);
}

static void cursor_pos(const BYTE *p, unsigned n)
{
    IivCursorPos pos;
    if (n < sizeof(pos)) return;
    memcpy(&pos, p, sizeof(pos));
    /* 相手の範囲の座標 → 映像の座標 */
    g_rm.ptrX = g_rm.deskW > 0 ? (int)((double)pos.x * g_rm.w / g_rm.deskW) : pos.x;
    g_rm.ptrY = g_rm.deskH > 0 ? (int)((double)pos.y * g_rm.h / g_rm.deskH) : pos.y;
    PostMessageW(g_notify, WM_APP_POINTER, 0, 0);
}

static BOOL message_loop(void)
{
    for (;;) {
        unsigned len, n;
        BYTE type, *p;
        if (!rd(&len, 4)) return FALSE;
        if (len < 1 || len > IIV_MAX_MSG) { set_error(L"壊れたメッセージを受け取りました(長さ %u)。", len); return FALSE; }
        p = rd_ptr((int)len);
        if (!p) return FALSE;
        type = p[0];
        p++;
        n = len - 1;
        switch (type) {
        case IIV_S_VIDEO_CONFIG:
            if (n >= sizeof(IivVideoConfig)) {
                IivVideoConfig vc;
                memcpy(&vc, p, sizeof(vc));
                if (!video_config(&vc)) return FALSE;
            }
            break;
        case IIV_S_VIDEO:
            if (!video_frame(p, n)) return FALSE;
            break;
        case IIV_S_CURSOR_SHAPE:
            cursor_shape(p, n);
            break;
        case IIV_S_CURSOR_POS:
            cursor_pos(p, n);
            break;
        case IIV_S_CLIPBOARD:
            if (!g_p.viewOnly) {
                WCHAR *w = utf8_to_utf16((const char *)p, (int)n);
                if (w && !PostMessageW(g_notify, WM_APP_SETCLIP, 0, (LPARAM)w)) free(w);
            }
            break;
        case IIV_S_FX:
            if (n >= 1) {
                switch (p[0]) {
                case FX_FILES: if (fx_usable()) fx_offer_received(g_connGen, p + 1, (int)n - 1); break;
                case FX_READ:  if (fx_usable()) fx_request(g_connGen, p + 1, (int)n - 1); break;
                case FX_READMANY: if (fx_usable()) fx_request_many(g_connGen, p + 1, (int)n - 1); break;
                case FX_DATA:  fx_deliver(g_connGen, p + 1, (int)n - 1); break;
                case FX_DATAZ: fx_deliver_z(g_connGen, p + 1, (int)n - 1); break;
                }
            }
            break;
        case IIV_S_PING:
            if (n >= sizeof(IivPing)) send_msg(IIV_C_PONG, p, sizeof(IivPing), NULL, 0);
            break;
        case IIV_S_AUDIO_CONFIG:
            if (n >= sizeof(IivAudioConfig) && g_p.audio) {
                IivAudioConfig ac;
                memcpy(&ac, p, sizeof(ac));
                if (ac.codec == g_p.audio) audio_on_config(&ac);
            }
            break;
        case IIV_S_AUDIO:
            if (g_p.audio) audio_on_packet(p, n);
            break;
        default:
            break;                          /* 知らないものは読み飛ばす(新しい版の相手のため) */
        }
        rd_trim();
    }
}

/* ------------------------------------------------------------------ */
/*  接続                                                                */
/* ------------------------------------------------------------------ */

BOOL conn_parse_host(const WCHAR *in, WCHAR *host, int hostCap, int *port)
{
    WCHAR tmp[256], *p;
    lstrcpynW(tmp, in, ARRAYSIZE(tmp));
    for (p = tmp; *p == L' '; p++) ;
    {
        int n = lstrlenW(p);
        while (n && p[n - 1] == L' ') p[--n] = 0;
    }
    if (!*p) return FALSE;
    *port = IIV_DEFAULT_PORT;
    if (*p == L'[') {                   /* [IPv6]:port */
        WCHAR *e = wcschr(p, L']');
        if (!e) return FALSE;
        *e = 0;
        lstrcpynW(host, p + 1, hostCap);
        if (e[1] == L':') *port = _wtoi(e + 2 + (e[2] == L':'));
        return *port > 0 && *port < 65536;
    }
    {
        WCHAR *dc = wcsstr(p, L"::"), *c = wcschr(p, L':');
        if (c && wcschr(c + 1, L':') && !dc) {   /* 括弧なしの IPv6 */
            lstrcpynW(host, p, hostCap);
            return TRUE;
        }
        if (dc && !wcschr(dc + 2, L':')) {        /* host::port(iivnc と同じ書き方も受け付ける) */
            *dc = 0;
            *port = _wtoi(dc + 2);
        } else if (c && !wcschr(c + 1, L':')) {  /* host:port */
            *c = 0;
            *port = _wtoi(c + 1);
        }
    }
    lstrcpynW(host, p, hostCap);
    if (!host[0]) lstrcpyW(host, L"localhost");
    return *port > 0 && *port < 65536;
}

static SOCKET connect_to(const WCHAR *host, int port)
{
    ADDRINFOW hints, *res = NULL, *ai;
    WCHAR     ps[16];
    SOCKET    s = INVALID_SOCKET;
    int       err;
    ZeroMemory(&hints, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    wsprintfW(ps, L"%d", port);
    err = GetAddrInfoW(host, ps, &hints, &res);
    if (err) {
        set_error(L"「%s」が見つかりません。名前かアドレスを確かめてください。", host);
        return INVALID_SOCKET;
    }
    for (ai = res; ai && !g_stop; ai = ai->ai_next) {
        u_long nb = 1;
        fd_set ws, es;
        struct timeval tv = { 10, 0 };
        s = socket(ai->ai_family, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) continue;
        ioctlsocket(s, FIONBIO, &nb);
        if (connect(s, ai->ai_addr, (int)ai->ai_addrlen) == 0) goto ok;
        if (WSAGetLastError() == WSAEWOULDBLOCK) {
            FD_ZERO(&ws); FD_SET(s, &ws);
            FD_ZERO(&es); FD_SET(s, &es);
            if (select(0, NULL, &ws, &es, &tv) > 0 && FD_ISSET(s, &ws)) goto ok;
        }
        closesocket(s);
        s = INVALID_SOCKET;
        continue;
    ok:
        nb = 0;
        ioctlsocket(s, FIONBIO, &nb);
        break;
    }
    FreeAddrInfoW(res);
    if (s == INVALID_SOCKET && !g_stop)
        set_error(L"%s のポート %d につながりません。\nサーバーが動いているか、ファイアウォールで止められていないか確かめてください。", host, port);
    return s;
}

static DWORD WINAPI conn_thread(void *arg)
{
    WCHAR *reason = NULL;
    (void)arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    g_sock = connect_to(g_p.host, g_p.port);
    if (g_sock != INVALID_SOCKET) {
        int one = 1, big = 4 << 20;
        setsockopt(g_sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
        setsockopt(g_sock, SOL_SOCKET, SO_RCVBUF, (const char *)&big, sizeof(big));
        if (handshake()) {
            InterlockedExchange(&g_active, 1);
            log_printf(L"%s:%d に接続した(「%s」%s)", g_p.host, g_p.port, g_rm.name, g_p.viewOnly ? L"、見るだけ" : L"");
            if (g_p.quality != Q_AUTO) conn_set_quality(g_p.quality);
            if (g_p.audio) conn_set_audio(g_p.audio);
            PostMessageW(g_notify, WM_APP_CONNECTED, 0, 0);
            if (g_fxOffer && fx_usable()) PostMessageW(g_notify, WM_APP_FXOFFER, 0, 0);
            message_loop();
            InterlockedExchange(&g_active, 0);
            InterlockedExchange(&g_fxOK, 0);
            fx_conn_closed(g_connGen);
        }
    }
    vdec_close();
    g_vdecOk = FALSE;
    audio_stop();
    if (g_err[0] && !g_stop) {
        size_t n = wcslen(g_err) + 1;
        reason = (WCHAR *)malloc(n * sizeof(WCHAR));
        if (reason) memcpy(reason, g_err, n * sizeof(WCHAR));
    }
    log_printf(L"切れた: %s", g_err[0] ? g_err : L"(相手が閉じた)");
    EnterCriticalSection(&g_sendCs);
    if (g_sock != INVALID_SOCKET) closesocket(g_sock);
    g_sock = INVALID_SOCKET;
    LeaveCriticalSection(&g_sendCs);
    if (!g_stop) PostMessageW(g_notify, WM_APP_CLOSED, 0, (LPARAM)reason);
    else free(reason);
    CoUninitialize();
    return 0;
}

void conn_start(const ConnParams *p, HWND notify)
{
    if (!g_inited) {
        WSADATA wd;
        WSAStartup(MAKEWORD(2, 2), &wd);
        InitializeCriticalSection(&g_sendCs);
        InitializeSRWLock(&g_rm.lock);
        g_inited = TRUE;
    }
    conn_stop();
    g_p = *p;
    g_notify = notify;
    g_stop = 0;
    g_err[0] = 0;
    g_authFailed = g_needPw = FALSE;
    InterlockedIncrement(&g_connGen);
    InterlockedExchange(&g_fxOK, 0);
    g_rbPos = g_rbLen = 0;
    g_rm.updates = 0;
    g_rm.bytes = 0;
    g_rm.decodeTicks = 0;
    g_rm.haveCursorEnc = FALSE;
    g_rm.framePosted = FALSE;
    audio_reset_status();
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpf = f.QuadPart;
    }
    g_thread = CreateThread(NULL, 0, conn_thread, NULL, 0, NULL);
}

void conn_stop(void)
{
    if (!g_thread) return;
    InterlockedExchange(&g_stop, 1);
    EnterCriticalSection(&g_sendCs);
    if (g_sock != INVALID_SOCKET) shutdown(g_sock, SD_BOTH);
    LeaveCriticalSection(&g_sendCs);
    WaitForSingleObject(g_thread, 5000);
    CloseHandle(g_thread);
    g_thread = NULL;
    InterlockedExchange(&g_active, 0);
}

BOOL conn_active(void) { return g_active != 0; }
const WCHAR *conn_last_error(void) { return g_err; }
BOOL conn_auth_failed(void) { return g_authFailed; }
BOOL conn_needs_password(void) { return g_needPw; }
