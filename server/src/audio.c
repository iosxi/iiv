/* ==================================================================
 * audio.c - 「音を鳴らす」: この PC で鳴っている音を取り込んで配る
 *
 *  設定の「音を鳴らす」(audio=1)がオンで、音を求める相手(IIV_C_AUDIO)が 1 人でも
 *  いるときだけ、取り込みのスレッドを起こす。誰も求めなくなったら 2 秒でスレッドは
 *  自分で終わる。音を求める相手がいなければ、映像の側には何も足さない。
 *
 *  取り込み: 既定の再生デバイスのループバック(WASAPI、共有モード、ミックスの形のまま)を
 *  10ms ごとに読む。イベントで待つ形は、パケットの間が 112ms 空くことがあった
 *  (2026-10-07 実測。10ms ごとに読むと最大 24ms)。何も鳴っていないとパケットは来ない
 *  (そのときは何も送らない)。形の変換(AUTOCONVERTPCM)をさせると 2 秒で 1/4 しか
 *  来なかったので、変換はここでする: float / 16 / 24 / 32bit → 16bit、多チャンネル → ステレオ、
 *  44.1kHz と 48kHz 以外 → 48kHz(線形補間)。
 *  既定のデバイスが変わったら(2 秒ごとに調べる)、取り込み直す。
 *
 *  配る: 相手ごとの待ち行列(Client.aq)に入れ、書き手(server.c)が映像より先に送る。
 *   PCM  10ms ずつ。無音のパケット(AUDCLNT_BUFFERFLAGS_SILENT)は中身なしで送る。
 *   AAC  aac.c で 96kbps に符号化し、1024 サンプルずつ。音が途切れたら(60ms 何も来ない)、
 *        エンコーダに残った分を無音で押し出す(再開したときに古い音が出ないように)。
 *
 *  検証用 -testaudio: 取り込む代わりに、48kHz で 左 = 2 秒で 200Hz → 8kHz のスイープ(繰り返し)、
 *  右 = 1kHz の正弦波を作る(振幅 0.3)。
 * ================================================================== */

#include "iiv.h"
#include "wasapi.h"
#include "aac.h"
#include <math.h>
#include <avrt.h>
#include <timeapi.h>

#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "winmm.lib")

#define AAC_BYTES_PER_SEC 12000         /* 96kbps(128kbps と SNR は 0.7dB しか違わなかった) */
#define TICK_MS           10

static CRITICAL_SECTION g_acs;          /* 下の 4 つと、スレッドの始まり・終わり */
static BOOL           g_inited;
static HANDLE         g_thread;
static BOOL           g_running;
static volatile LONG  g_stop;

/* 今の形(g_acs で守る) */
static int            g_rate;           /* 0 = まだ取り込んでいない */
static int            g_status = IIV_AS_OK;
static unsigned short g_asc;            /* AAC のエンコーダを開いたら */
static LONG           g_fmtVer = 1;     /* 形が変わるたびに増やす */

static LONG64         g_qpf;

/* ------------------------------------------------------------------ */
/*  配るフレーム                                                        */
/* ------------------------------------------------------------------ */

static AFrame *aframe_new(int codec, UINT32 seq, UINT32 frames, LONG64 qpc, const void *data, int len)
{
    AFrame *f = (AFrame *)malloc(sizeof(AFrame) + (size_t)len);
    if (!f) return NULL;
    f->ref = 1;
    f->codec = codec;
    f->seq = seq;
    f->frames = frames;
    f->qpc = qpc;
    f->len = len;
    if (len) memcpy(f->data, data, (size_t)len);
    return f;
}

void aframe_release(AFrame *f)
{
    if (f && !InterlockedDecrement(&f->ref)) free(f);
}

/* ------------------------------------------------------------------ */
/*  誰が何を求めているか                                                */
/* ------------------------------------------------------------------ */

static void wants(BOOL *pcm, BOOL *aac)
{
    Client *c;
    *pcm = *aac = FALSE;
    if (!g_cfg.audio) return;
    AcquireSRWLockShared(&g_scr.lock);
    for (c = g_scr.clients; c; c = c->next) {
        if (!c->active || c->quit) continue;
        if (c->audioWanted == IIV_AUDIO_PCM) *pcm = TRUE;
        else if (c->audioWanted == IIV_AUDIO_AAC) *aac = TRUE;
    }
    ReleaseSRWLockShared(&g_scr.lock);
}

static DWORD WINAPI audio_thread(void *arg);

/* 相手の求めや設定が変わった: 要るならスレッドを起こす(要らなくなったらスレッドが自分で終わる) */
void audio_update(void)
{
    static int lastCfg = -1;
    BOOL pcm, aac;
    if (!g_inited) return;
    wants(&pcm, &aac);
    EnterCriticalSection(&g_acs);
    if (lastCfg != g_cfg.audio) {               /* 設定の「音を鳴らす」が変わった: 相手に知らせ直す */
        lastCfg = g_cfg.audio;
        g_fmtVer++;
    }
    if ((pcm || aac) && !g_running && !g_stop) {
        if (g_thread) { CloseHandle(g_thread); g_thread = NULL; }
        g_thread = CreateThread(NULL, 0, audio_thread, NULL, 0, NULL);
        g_running = g_thread != NULL;
    }
    LeaveCriticalSection(&g_acs);
}

/* 相手(codec を求めている)に送る IIV_S_AUDIO_CONFIG。まだ形が決まっていなければ FALSE */
BOOL audio_config_for(int codec, IivAudioConfig *ac, LONG *ver)
{
    BOOL ok = TRUE;
    ZeroMemory(ac, sizeof(*ac));
    ac->codec = (unsigned char)codec;
    ac->channels = 2;
    EnterCriticalSection(&g_acs);
    *ver = g_fmtVer * 4 + codec;
    if (!g_cfg.audio) ac->status = IIV_AS_DISABLED;
    else if (g_status != IIV_AS_OK) ac->status = (unsigned char)g_status;
    else if (!g_rate || (codec == IIV_AUDIO_AAC && !g_asc)) ok = FALSE;
    ac->rate = (unsigned)g_rate;
    ac->asc = g_asc;
    LeaveCriticalSection(&g_acs);
    return ok;
}

static void set_format(int rate, int status, unsigned short asc)
{
    EnterCriticalSection(&g_acs);
    if (rate != g_rate || status != g_status || asc != g_asc) {
        g_rate = rate;
        g_status = status;
        g_asc = asc;
        g_fmtVer++;
    }
    LeaveCriticalSection(&g_acs);
}

/* ------------------------------------------------------------------ */
/*  取り込み(WASAPI のループバック)                                     */
/* ------------------------------------------------------------------ */

typedef struct Capture {
    IMMDeviceEnumerator *en;
    IAudioClient        *ac;
    IAudioCaptureClient *cc;
    WCHAR               *devId;         /* CoTaskMemAlloc */
    int    rate, channels, bytes;       /* 元の形 */
    BOOL   isFloat;
    int    container;                   /* 1 サンプルのバイト数 */
    float  wl[8], wr[8];                /* チャンネル → 左右の重み */
    int    outRate;                     /* 送る速さ(44100 か 48000) */
    double rsPos;                       /* 速さを変えるときの位置(元のサンプルの単位) */
    float  rsPrevL, rsPrevR;
    BOOL   rsHavePrev;
} Capture;

static void cap_close(Capture *cp)
{
    if (cp->ac) IAudioClient_Stop(cp->ac);
    if (cp->cc) { IAudioCaptureClient_Release(cp->cc); cp->cc = NULL; }
    if (cp->ac) { IAudioClient_Release(cp->ac); cp->ac = NULL; }
    if (cp->devId) { CoTaskMemFree(cp->devId); cp->devId = NULL; }
}

/* チャンネルの並び(dwChannelMask)から、左右へ混ぜる重みを決める */
static void cap_weights(Capture *cp, DWORD mask)
{
    int i, bit = 0;
    for (i = 0; i < 8; i++) cp->wl[i] = cp->wr[i] = 0;
    if (cp->channels == 1) { cp->wl[0] = cp->wr[0] = 1; return; }
    if (!mask) mask = cp->channels == 2 ? 3 : (1u << cp->channels) - 1;
    for (i = 0; i < cp->channels && i < 8; i++) {
        DWORD sp;
        while (bit < 32 && !(mask & (1u << bit))) bit++;
        sp = bit < 32 ? (1u << bit) : 0;
        bit++;
        switch (sp) {
        case SPEAKER_FRONT_LEFT:  cp->wl[i] = 1; break;
        case SPEAKER_FRONT_RIGHT: cp->wr[i] = 1; break;
        case SPEAKER_FRONT_CENTER: cp->wl[i] = cp->wr[i] = 0.707f; break;
        case SPEAKER_BACK_LEFT: case SPEAKER_SIDE_LEFT: case SPEAKER_FRONT_LEFT_OF_CENTER: cp->wl[i] = 0.707f; break;
        case SPEAKER_BACK_RIGHT: case SPEAKER_SIDE_RIGHT: case SPEAKER_FRONT_RIGHT_OF_CENTER: cp->wr[i] = 0.707f; break;
        case SPEAKER_BACK_CENTER: cp->wl[i] = cp->wr[i] = 0.5f; break;
        default: break;                         /* LFE などは混ぜない */
        }
    }
    if (cp->channels > 2) {                     /* 混ぜて大きくなりすぎないよう、左の重みの和で割る */
        float s = 0;
        for (i = 0; i < cp->channels && i < 8; i++) s += cp->wl[i];
        if (s > 1) for (i = 0; i < 8; i++) { cp->wl[i] /= s; cp->wr[i] /= s; }
    }
}

static BOOL cap_open(Capture *cp)
{
    IMMDevice    *dev = NULL;
    WAVEFORMATEX *mix = NULL;
    HRESULT hr;
    cap_close(cp);
    if (!cp->en && FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **)&cp->en))) return FALSE;
    if (FAILED(hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(cp->en, eRender, eConsole, &dev))) {
        log_printf(L"音: 再生デバイスが無い (0x%08lX)", hr);
        return FALSE;
    }
    IMMDevice_GetId(dev, &cp->devId);
    hr = IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&cp->ac);
    IMMDevice_Release(dev);
    if (FAILED(hr) || FAILED(hr = IAudioClient_GetMixFormat(cp->ac, &mix))) goto fail;
    cp->rate = (int)mix->nSamplesPerSec;
    cp->channels = mix->nChannels;
    cp->container = mix->wBitsPerSample / 8;
    cp->isFloat = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    cp->bytes = mix->nBlockAlign;
    if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const WAVEFORMATEXTENSIBLE *e = (const WAVEFORMATEXTENSIBLE *)mix;
        cp->isFloat = IsEqualGUID(&e->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        cap_weights(cp, e->dwChannelMask);
    } else {
        cap_weights(cp, 0);
    }
    if (cp->channels < 1 || cp->channels > 8 || !(cp->isFloat ? cp->container == 4 : (cp->container >= 2 && cp->container <= 4))) {
        log_printf(L"音: 取り込めない形(%d ch、%d bit、%s)", cp->channels, mix->wBitsPerSample, cp->isFloat ? L"float" : L"整数");
        hr = E_FAIL;
        goto fail;
    }
    /* 100ms の器(10ms ごとに読むので余裕を持たせる) */
    if (FAILED(hr = IAudioClient_Initialize(cp->ac, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 1000000, 0, mix, NULL)) ||
        FAILED(hr = IAudioClient_GetService(cp->ac, &IID_IAudioCaptureClient, (void **)&cp->cc)) ||
        FAILED(hr = IAudioClient_Start(cp->ac))) goto fail;
    cp->outRate = (cp->rate == 44100 || cp->rate == 48000) ? cp->rate : 48000;
    cp->rsPos = 0;
    cp->rsHavePrev = FALSE;
    log_printf(L"音: 取り込みを始めた(%d Hz、%d ch、%d bit %s → %d Hz、16bit、ステレオ)", cp->rate, cp->channels,
               mix->wBitsPerSample, cp->isFloat ? L"float" : L"整数", cp->outRate);
    CoTaskMemFree(mix);
    return TRUE;
fail:
    log_printf(L"音: 取り込めない (0x%08lX)", hr);
    CoTaskMemFree(mix);
    cap_close(cp);
    return FALSE;
}

/* 既定の再生デバイスが変わったか */
static BOOL cap_device_changed(Capture *cp)
{
    IMMDevice *dev = NULL;
    WCHAR *id = NULL;
    BOOL changed = FALSE;
    if (!cp->en || FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(cp->en, eRender, eConsole, &dev))) return cp->devId != NULL;
    if (SUCCEEDED(IMMDevice_GetId(dev, &id))) changed = !cp->devId || lstrcmpW(id, cp->devId) != 0;
    CoTaskMemFree(id);
    IMMDevice_Release(dev);
    return changed;
}

static float sample(const Capture *cp, const BYTE *p)
{
    switch (cp->isFloat ? 0 : cp->container) {
    case 0: return *(const float *)p;
    case 2: return *(const short *)p / 32768.0f;
    case 3: return (float)((int)((UINT32)p[0] << 8 | (UINT32)p[1] << 16 | (UINT32)p[2] << 24) >> 8) / 8388608.0f;
    default: return (float)(*(const int *)p / 2147483648.0);
    }
}

static short to16(float v)
{
    int x = (int)lrintf(v * 32767.0f);
    return (short)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
}

/* 元の形の n サンプル → 16bit ステレオ(送る速さ)。書いた数を返す */
static int cap_convert(Capture *cp, const BYTE *src, int n, BOOL silent, short *dst, int cap)
{
    int i, k, out = 0;
    if (cp->outRate == cp->rate) {
        for (i = 0; i < n && out < cap; i++, out++) {
            float l = 0, r = 0;
            if (!silent) {
                const BYTE *f = src + (size_t)i * cp->bytes;
                for (k = 0; k < cp->channels && k < 8; k++) {
                    float v = sample(cp, f + (size_t)k * cp->container);
                    l += v * cp->wl[k];
                    r += v * cp->wr[k];
                }
            }
            dst[out * 2] = to16(l);
            dst[out * 2 + 1] = to16(r);
        }
        return out;
    }
    /* 速さを変える(線形補間)。rsPos は「前のサンプル」からの位置 */
    {
        double step = (double)cp->rate / cp->outRate;
        for (i = 0; i < n; i++) {
            float l = 0, r = 0;
            if (!silent) {
                const BYTE *f = src + (size_t)i * cp->bytes;
                for (k = 0; k < cp->channels && k < 8; k++) {
                    float v = sample(cp, f + (size_t)k * cp->container);
                    l += v * cp->wl[k];
                    r += v * cp->wr[k];
                }
            }
            if (!cp->rsHavePrev) { cp->rsPrevL = l; cp->rsPrevR = r; cp->rsHavePrev = TRUE; cp->rsPos = 0; continue; }
            while (cp->rsPos < 1.0 && out < cap) {
                float t = (float)cp->rsPos;
                dst[out * 2] = to16(cp->rsPrevL + (l - cp->rsPrevL) * t);
                dst[out * 2 + 1] = to16(cp->rsPrevR + (r - cp->rsPrevR) * t);
                out++;
                cp->rsPos += step;
            }
            cp->rsPos -= 1.0;
            cp->rsPrevL = l;
            cp->rsPrevR = r;
        }
    }
    return out;
}

/* ------------------------------------------------------------------ */
/*  検証用の音                                                          */
/* ------------------------------------------------------------------ */

static int test_generate(UINT64 *pos, int n, short *dst)
{
    int i;
    for (i = 0; i < n; i++) {
        double t = (double)((*pos) % 96000) / 48000.0, tt = (double)(*pos) / 48000.0;
        dst[i * 2] = (short)(0.3 * 32767 * sin(2 * 3.14159265358979 * (200 * t + (8000 - 200) * t * t / 4)));
        dst[i * 2 + 1] = (short)(0.3 * 32767 * sin(2 * 3.14159265358979 * 1000 * tt));
        (*pos)++;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/*  スレッド                                                            */
/* ------------------------------------------------------------------ */

typedef struct Out {
    UINT32 seqPcm, seqAac;
    UINT64 aacIn, aacOut;               /* エンコーダへ入れた・出てきたサンプル数 */
    LONG64 qpcAt[64];                   /* 入れたサンプルの位置 → 取り込んだ時刻(1024 ごと) */
} Out;

static void deliver(AFrame *f)
{
    if (f) {
        server_audio_deliver(f);
        aframe_release(f);
    }
}

static void on_aac(void *ctx, const BYTE *p, int n)
{
    Out *o = (Out *)ctx;
    LONG64 qpc = o->qpcAt[(o->aacOut / 1024) % 64];
    deliver(aframe_new(IIV_AUDIO_AAC, ++o->seqAac, 1024, qpc, p, n));
    o->aacOut += 1024;
}

static void put_aac(AacEnc *enc, Out *o, const short *pcm, int n, LONG64 qpc)
{
    UINT64 a = o->aacIn, b = a + (UINT64)n, k;
    /* 1024 の切れ目ごとに、その位置の時刻を覚える */
    for (k = (a + 1023) / 1024 * 1024; k < b; k += 1024)
        o->qpcAt[(k / 1024) % 64] = qpc + (LONG64)((k - a) * (UINT64)g_qpf / (UINT64)(g_rate ? g_rate : 48000));
    o->aacIn = b;
    aacenc_put(enc, pcm, n, on_aac, o);
}

static DWORD WINAPI audio_thread(void *arg)
{
    Capture  cp;
    Out      o;
    AacEnc  *enc = NULL;
    HANDLE   timer, task = NULL;
    BOOL     hiRes = TRUE, capOk = FALSE;
    DWORD    lastTry = 0, lastDevCheck = GetTickCount(), idleSince = 0, lastData = GetTickCount();
    BOOL     aacDirty = FALSE;
    short   *buf = (short *)malloc(48000 * 4);          /* 1 秒分 */
    UINT64   testPos = 0;
    LONG64   testNext = 0;
    LARGE_INTEGER li;
    DWORD    taskIdx = 0;
    (void)arg;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    ZeroMemory(&cp, sizeof(cp));
    ZeroMemory(&o, sizeof(o));
    task = AvSetMmThreadCharacteristicsW(L"Audio", &taskIdx);
    timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) {                       /* Windows 10 1803 より前 */
        hiRes = FALSE;
        timeBeginPeriod(1);
        timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    }
    li.QuadPart = -(LONG64)TICK_MS * 10000;
    SetWaitableTimer(timer, &li, TICK_MS, NULL, NULL, FALSE);
    log_printf(L"音: スレッドを始めた%s", g_testAudio ? L"(検証用の音)" : L"");
    QueryPerformanceCounter(&li);
    testNext = li.QuadPart;

    while (!g_stop && buf) {
        BOOL wantPcm, wantAac;
        int  n = 0;
        BOOL silent = TRUE;
        LONG64 qpc = 0;
        DWORD now;

        WaitForSingleObject(timer, 100);
        if (g_stop) break;
        now = GetTickCount();
        wants(&wantPcm, &wantAac);
        if (!wantPcm && !wantAac) {
            if (!idleSince) idleSince = now;
            if (now - idleSince > 2000) {
                EnterCriticalSection(&g_acs);
                wants(&wantPcm, &wantAac);              /* 終わる直前にもう一度(その間に来た相手のため) */
                if (!wantPcm && !wantAac) g_running = FALSE;
                LeaveCriticalSection(&g_acs);
                if (!g_running) break;
            }
            continue;
        }
        idleSince = 0;

        /* AAC のエンコーダは、求める相手がいる間だけ */
        if (wantAac && !enc && g_rate) {
            enc = aacenc_open(g_rate, AAC_BYTES_PER_SEC);
            if (enc) {
                set_format(g_rate, g_status, aacenc_asc(enc));
                o.aacIn = o.aacOut = 0;
                log_printf(L"音: AAC のエンコーダを開いた(%d Hz、%d kbps、ASC 0x%04X)", g_rate, AAC_BYTES_PER_SEC * 8 / 1000, aacenc_asc(enc));
            }
        } else if (!wantAac && enc) {
            aacenc_close(enc);
            enc = NULL;
            set_format(g_rate, g_status, 0);
        }

        if (g_testAudio) {
            /* 検証用: 経った時間の分だけ作る */
            QueryPerformanceCounter(&li);
            n = (int)((li.QuadPart - testNext) * 48000 / g_qpf);
            if (n > 4800) { n = 4800; testNext = li.QuadPart; }
            if (n > 0) {
                qpc = li.QuadPart - (LONG64)n * g_qpf / 48000;
                testNext += (LONG64)n * g_qpf / 48000;
                test_generate(&testPos, n, buf);
                silent = FALSE;
            }
            if (!g_rate) set_format(48000, IIV_AS_OK, g_asc);
        } else {
            if (!capOk && (!lastTry || now - lastTry > 2000)) {
                lastTry = now;
                capOk = cap_open(&cp);
                if (capOk) {
                    if (enc && cp.outRate != g_rate) { aacenc_close(enc); enc = NULL; }
                    set_format(cp.outRate, IIV_AS_OK, enc ? aacenc_asc(enc) : 0);
                } else {
                    set_format(0, IIV_AS_NODEVICE, 0);
                }
            }
            if (capOk && now - lastDevCheck > 2000) {
                lastDevCheck = now;
                if (cap_device_changed(&cp)) {
                    log_printf(L"音: 既定の再生デバイスが変わった。取り込み直す");
                    capOk = FALSE;
                    lastTry = 0;
                    continue;
                }
            }
            while (capOk) {
                UINT32 next = 0, got = 0;
                BYTE  *p = NULL;
                DWORD  fl = 0;
                UINT64 qpos = 0;
                HRESULT hr = IAudioCaptureClient_GetNextPacketSize(cp.cc, &next);
                if (SUCCEEDED(hr) && !next) break;
                if (SUCCEEDED(hr)) hr = IAudioCaptureClient_GetBuffer(cp.cc, &p, &got, &fl, NULL, &qpos);
                if (FAILED(hr)) {
                    log_printf(L"音: 取り込みが止まった (0x%08lX)。取り込み直す", hr);
                    capOk = FALSE;
                    lastTry = now;
                    cap_close(&cp);
                    break;
                }
                if (!n) qpc = (LONG64)(qpos * (UINT64)g_qpf / 10000000);
                if (!(fl & AUDCLNT_BUFFERFLAGS_SILENT)) silent = FALSE;
                n += cap_convert(&cp, p, (int)got, (fl & AUDCLNT_BUFFERFLAGS_SILENT) != 0, buf + (size_t)n * 2, 48000 - n);
                IAudioCaptureClient_ReleaseBuffer(cp.cc, got);
                if (n >= 48000 - 4800) break;
            }
        }

        if (n > 0) {
            lastData = now;
            if (wantPcm) deliver(aframe_new(IIV_AUDIO_PCM, ++o.seqPcm, (UINT32)n, qpc, silent ? NULL : buf, silent ? 0 : n * 4));
            if (wantAac && enc) {
                if (silent) ZeroMemory(buf, (size_t)n * 4);
                put_aac(enc, &o, buf, n, qpc);
                aacDirty = TRUE;
            }
        } else if (enc && aacDirty && now - lastData > 60) {
            /* 音が途切れた: エンコーダに残った分(約 70ms)を無音で押し出す */
            ZeroMemory(buf, 4096 * 4);
            QueryPerformanceCounter(&li);
            put_aac(enc, &o, buf, 4096, li.QuadPart);
            aacDirty = FALSE;
        }
    }

    if (enc) aacenc_close(enc);
    cap_close(&cp);
    if (cp.en) IMMDeviceEnumerator_Release(cp.en);
    set_format(0, IIV_AS_OK, 0);
    CancelWaitableTimer(timer);
    CloseHandle(timer);
    if (!hiRes) timeEndPeriod(1);
    if (task) AvRevertMmThreadCharacteristics(task);
    free(buf);
    CoUninitialize();
    log_printf(L"音: スレッドを終えた");
    EnterCriticalSection(&g_acs);
    g_running = FALSE;
    LeaveCriticalSection(&g_acs);
    return 0;
}

void audio_init(void)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpf = f.QuadPart;
    InitializeCriticalSection(&g_acs);
    g_inited = TRUE;
}

void audio_shutdown(void)
{
    HANDLE t;
    if (!g_inited) return;
    InterlockedExchange(&g_stop, 1);
    EnterCriticalSection(&g_acs);
    t = g_thread;
    g_thread = NULL;
    LeaveCriticalSection(&g_acs);
    if (t) {
        WaitForSingleObject(t, 3000);
        CloseHandle(t);
    }
}
