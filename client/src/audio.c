/* ==================================================================
 * audio.c - 「音を鳴らす」: サーバーで鳴っている音を、こちらで鳴らす
 *
 *  通信のスレッドは、受け取った音を写して待ち行列に入れるだけ(映像の復号を待たせない)。
 *  鳴らすのは専用のスレッド(MMCSS の "Pro Audio"):
 *   1. 待ち行列の音を取り出し、AAC なら復号して(aac.c)、16bit ステレオの輪(ring)に溜める
 *   2. 既定の再生デバイスへ WASAPI(共有モード、イベント駆動)で渡す。形はこちらのまま
 *      (16bit ステレオ、サーバーの速さ)で、デバイスの形への変換は Windows に任せる
 *      (AUTOCONVERTPCM。2026-10-07 に 44.1kHz・48kHz とも初期化できた)
 *  デバイスには 20ms 先までしか書かない(遅れを増やさない)。溜まり具合は輪とデバイスの合計で見て、
 *  目標(PCM 50ms、AAC 70ms)まで溜めてから鳴らし始め、空になったら(途切れた)また目標まで溜める。
 *  目標を輪だけで 30ms にしたときは、20ms がすぐデバイスへ移って輪に 10ms しか残らず、
 *  届く間隔の揺れ(13〜17ms。つないだ直後は映像のデコーダを開く間に 31ms 止まった)で途切れた(2026-10-07)。
 *
 *  時計のずれ: サーバーの取り込みとこちらの再生は別の時計で動くので、放っておくと輪が
 *  じわじわ増える(遅れが溜まる)か減る(途切れる)。輪の量の平均が目標からずれた分だけ、
 *  再生の速さを IAudioClockAdjustment で ±0.5% まで変えて戻す(RATEADJUST。高音質の変換は Windows)。
 *  使えないときは、1 回に 1 サンプル落とすか重ねる。
 *  鳴らし始めるとき(途切れた後も)と、輪が目標より 100ms 以上多いとき(通信が詰まった後など)は、
 *  古い分を捨てて目標に戻す(速さの調整では 100ms 戻すのに 20 秒かかる)。
 *
 *  検証用: -audiodump <wav> 輪に入れた音を WAV に書く、-audiomute 音量 0 で鳴らす(処理は同じ)。
 * ================================================================== */

#include "iivc.h"
#include "wasapi.h"
#include "aac.h"
#include <avrt.h>
#include <math.h>

#pragma comment(lib, "avrt.lib")

#define QUEUE_MAX   200                 /* 待ち行列の上限(PCM で 2 秒) */
#define DEV_MS      20                  /* デバイスに先に書いておく量 */
#define TARGET_PCM_MS 50                /* 溜めておく量(輪 + デバイス)。届く間隔の揺れ 30ms + デバイス 20ms */
#define TARGET_AAC_MS 70                /* AAC は 21ms ずつまとまって届くので、その分多く */
#define MAX_EXTRA_MS 100                /* 輪が目標をこれだけ超えたら捨てて戻す */
#define RATE_ADJ_MAX 0.005              /* 速さの調整の上限(±0.5%) */

typedef struct APkt {
    struct APkt *next;
    IivAudioHead h;
    LONG64 recvQpc;
    int    len;
    BYTE   data[1];
} APkt;

static CRITICAL_SECTION g_cs;           /* 待ち行列と g_newCfg */
static CRITICAL_SECTION g_life;         /* スレッドを始める・止める(通信のスレッドと画面のスレッドから) */
static BOOL           g_inited;
static APkt          *g_head, *g_tail;
static int            g_qn;
static IivAudioConfig g_newCfg;
static LONG           g_cfgSeq;
static HANDLE         g_thread, g_ev;
static volatile LONG  g_quit;
static volatile LONG  g_status = -1;    /* サーバーから最後に来た状態(IIV_AS_*。-1 = 来ていない) */
static LONG64         g_qpf;

/* 統計(スレッドが書き、audio_stats が読む) */
static volatile LONG  g_underruns, g_drops, g_lost, g_packets;
static volatile LONG  g_bufMs10, g_arriveMs10, g_ratePpm;   /* 0.1ms 単位、ppm */

/* ------------------------------------------------------------------ */
/*  通信のスレッドから                                                  */
/* ------------------------------------------------------------------ */

static void init_once(void)
{
    if (g_inited) return;
    InitializeCriticalSection(&g_cs);
    InitializeCriticalSection(&g_life);
    g_ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpf = f.QuadPart;
    }
    g_inited = TRUE;
}

static void free_queue(void)
{
    APkt *p, *n;
    EnterCriticalSection(&g_cs);
    p = g_head;
    g_head = g_tail = NULL;
    g_qn = 0;
    LeaveCriticalSection(&g_cs);
    for (; p; p = n) { n = p->next; free(p); }
}

static DWORD WINAPI audio_thread(void *arg);

void audio_on_config(const IivAudioConfig *ac)
{
    init_once();
    InterlockedExchange(&g_status, ac->status);
    log_printf(L"音: サーバーの形 %s、%u Hz、%u ch、状態 %s", ac->codec == IIV_AUDIO_AAC ? L"AAC" : L"PCM", ac->rate, ac->channels,
               ac->status == IIV_AS_OK ? L"OK" : ac->status == IIV_AS_DISABLED ? L"サーバーの「音を鳴らす」が切られている" : L"サーバーで取り込めない");
    if (ac->status != IIV_AS_OK || !ac->codec || ac->channels != 2 || ac->rate < 8000 || ac->rate > 192000) {
        audio_stop();
        return;
    }
    EnterCriticalSection(&g_cs);
    g_newCfg = *ac;
    g_cfgSeq++;
    LeaveCriticalSection(&g_cs);
    EnterCriticalSection(&g_life);
    if (!g_thread) {
        g_quit = 0;
        g_underruns = g_drops = g_lost = g_packets = 0;
        g_thread = CreateThread(NULL, 0, audio_thread, NULL, 0, NULL);
    }
    LeaveCriticalSection(&g_life);
    SetEvent(g_ev);
}

void audio_on_packet(const BYTE *p, unsigned n)
{
    APkt *k, *old = NULL;
    LARGE_INTEGER now;
    if (!g_thread || n < sizeof(IivAudioHead)) return;
    k = (APkt *)malloc(sizeof(APkt) + (n - sizeof(IivAudioHead)));
    if (!k) return;
    QueryPerformanceCounter(&now);
    memcpy(&k->h, p, sizeof(IivAudioHead));
    k->next = NULL;
    k->recvQpc = now.QuadPart;
    k->len = (int)(n - sizeof(IivAudioHead));
    memcpy(k->data, p + sizeof(IivAudioHead), (size_t)k->len);
    EnterCriticalSection(&g_cs);
    if (g_tail) g_tail->next = k; else g_head = k;
    g_tail = k;
    if (++g_qn > QUEUE_MAX) {           /* 鳴らす側が止まっている: 古いものを捨てる */
        old = g_head;
        g_head = old->next;
        g_qn--;
    }
    LeaveCriticalSection(&g_cs);
    free(old);
    SetEvent(g_ev);
}

void audio_stop(void)
{
    if (!g_inited) return;
    EnterCriticalSection(&g_life);
    if (g_thread) {
        InterlockedExchange(&g_quit, 1);
        SetEvent(g_ev);
        WaitForSingleObject(g_thread, 3000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    LeaveCriticalSection(&g_life);
    free_queue();
}

void audio_reset_status(void) { InterlockedExchange(&g_status, -1); }
int  audio_server_status(void) { return g_status; }
BOOL audio_playing(void) { return g_thread != NULL; }

void audio_stats(WCHAR *s, int cap)
{
    _snwprintf(s, (size_t)cap, L"音: パケット %ld、抜け %ld、途切れ %ld、捨てた %ld、溜め %.1fms、届くまで %.1fms、速さ %+.3f%%",
               g_packets, g_lost, g_underruns, g_drops, g_bufMs10 / 10.0, g_arriveMs10 / 10.0, g_ratePpm / 10000.0);
    s[cap - 1] = 0;
}

/* ------------------------------------------------------------------ */
/*  WAV に書く(検証用)                                                  */
/* ------------------------------------------------------------------ */

static HANDLE wav_open(const WCHAR *path, int rate)
{
    HANDLE f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    BYTE h[44];
    DWORD wr;
    if (f == INVALID_HANDLE_VALUE) return NULL;
    ZeroMemory(h, sizeof(h));
    memcpy(h, "RIFF", 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    *(DWORD *)(h + 16) = 16;
    *(WORD *)(h + 20) = 1;
    *(WORD *)(h + 22) = 2;
    *(DWORD *)(h + 24) = (DWORD)rate;
    *(DWORD *)(h + 28) = (DWORD)rate * 4;
    *(WORD *)(h + 32) = 4;
    *(WORD *)(h + 34) = 16;
    memcpy(h + 36, "data", 4);
    WriteFile(f, h, sizeof(h), &wr, NULL);
    return f;
}

static void wav_close(HANDLE f)
{
    DWORD size, wr, v;
    if (!f) return;
    size = GetFileSize(f, NULL);
    SetFilePointer(f, 4, NULL, FILE_BEGIN);
    v = size - 8;
    WriteFile(f, &v, 4, &wr, NULL);
    SetFilePointer(f, 40, NULL, FILE_BEGIN);
    v = size - 44;
    WriteFile(f, &v, 4, &wr, NULL);
    CloseHandle(f);
}

/* ------------------------------------------------------------------ */
/*  輪(16bit ステレオ)                                                  */
/* ------------------------------------------------------------------ */

typedef struct Ring {
    short *buf;
    int    cap, rd, n;                  /* サンプル数(1 チャンネルあたり) */
} Ring;

static void ring_push(Ring *r, const short *pcm, int frames)
{
    int i;
    if (frames > r->cap) { pcm += (size_t)(frames - r->cap) * 2; frames = r->cap; }
    if (r->n + frames > r->cap) {       /* あふれる: 古い分を捨てる */
        int drop = r->n + frames - r->cap;
        r->rd = (r->rd + drop) % r->cap;
        r->n -= drop;
    }
    for (i = 0; i < frames; i++) {
        int w = (r->rd + r->n + i) % r->cap;
        if (pcm) { r->buf[w * 2] = pcm[i * 2]; r->buf[w * 2 + 1] = pcm[i * 2 + 1]; }
        else r->buf[w * 2] = r->buf[w * 2 + 1] = 0;
    }
    r->n += frames;
}

static void ring_pop(Ring *r, short *out, int frames)
{
    int i;
    for (i = 0; i < frames; i++) {
        int k = (r->rd + i) % r->cap;
        out[i * 2] = r->buf[k * 2];
        out[i * 2 + 1] = r->buf[k * 2 + 1];
    }
    r->rd = (r->rd + frames) % r->cap;
    r->n -= frames;
}

static void ring_skip(Ring *r, int frames)
{
    if (frames > r->n) frames = r->n;
    r->rd = (r->rd + frames) % r->cap;
    r->n -= frames;
}

/* ------------------------------------------------------------------ */
/*  再生                                                                */
/* ------------------------------------------------------------------ */

typedef struct Render {
    IAudioClient          *ac;
    IAudioRenderClient    *rc;
    IAudioClockAdjustment *adj;
    UINT32 bufFrames;
    HANDLE ev;
    int    rate;
    double speed;                       /* 今の速さの倍率(1.0 = そのまま) */
} Render;

static void render_close(Render *r)
{
    if (r->ac) IAudioClient_Stop(r->ac);
    if (r->adj) { IAudioClockAdjustment_Release(r->adj); r->adj = NULL; }
    if (r->rc) { IAudioRenderClient_Release(r->rc); r->rc = NULL; }
    if (r->ac) { IAudioClient_Release(r->ac); r->ac = NULL; }
}

static BOOL render_open(Render *r, int rate)
{
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    WAVEFORMATEX f;
    HRESULT hr;
    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    render_close(r);
    r->rate = rate;
    r->speed = 1.0;
    ZeroMemory(&f, sizeof(f));
    f.wFormatTag = WAVE_FORMAT_PCM;
    f.nChannels = 2;
    f.nSamplesPerSec = (DWORD)rate;
    f.wBitsPerSample = 16;
    f.nBlockAlign = 4;
    f.nAvgBytesPerSec = (DWORD)rate * 4;
    if (FAILED(hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **)&en))) goto fail;
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    IMMDeviceEnumerator_Release(en);
    if (FAILED(hr)) goto fail;
    hr = IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&r->ac);
    IMMDevice_Release(dev);
    if (FAILED(hr)) goto fail;
    /* 速さを変えられる形で開いてみる。駄目なら変えない形で(サンプルを落とす・重ねる) */
    hr = IAudioClient_Initialize(r->ac, AUDCLNT_SHAREMODE_SHARED, flags | AUDCLNT_STREAMFLAGS_RATEADJUST, 500000, 0, &f, NULL);
    if (SUCCEEDED(hr)) {
        if (FAILED(IAudioClient_GetService(r->ac, &IID_IAudioClockAdjustment, (void **)&r->adj))) r->adj = NULL;
    } else {
        IAudioClient_Release(r->ac);
        r->ac = NULL;
        dev = NULL;
        if (FAILED(hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **)&en))) goto fail;
        hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
        IMMDeviceEnumerator_Release(en);
        if (FAILED(hr)) goto fail;
        hr = IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&r->ac);
        IMMDevice_Release(dev);
        if (FAILED(hr)) goto fail;
        if (FAILED(hr = IAudioClient_Initialize(r->ac, AUDCLNT_SHAREMODE_SHARED, flags, 500000, 0, &f, NULL))) goto fail;
    }
    if (FAILED(hr = IAudioClient_SetEventHandle(r->ac, r->ev)) ||
        FAILED(hr = IAudioClient_GetBufferSize(r->ac, &r->bufFrames)) ||
        FAILED(hr = IAudioClient_GetService(r->ac, &IID_IAudioRenderClient, (void **)&r->rc))) goto fail;
    if (g_audioMute) {
        ISimpleAudioVolume *v = NULL;
        if (SUCCEEDED(IAudioClient_GetService(r->ac, &IID_ISimpleAudioVolume, (void **)&v))) {
            ISimpleAudioVolume_SetMasterVolume(v, 0.0f, NULL);
            ISimpleAudioVolume_Release(v);
        }
    }
    if (FAILED(hr = IAudioClient_Start(r->ac))) goto fail;
    log_printf(L"音: 鳴らし始めた(%d Hz、デバイスの器 %u サンプル、速さの調整 %s%s)", rate, r->bufFrames,
               r->adj ? L"あり" : L"なし(サンプルを落とす・重ねる)", g_audioMute ? L"、音量 0(検証用)" : L"");
    return TRUE;
fail:
    log_printf(L"音: 鳴らせない (0x%08lX)", hr);
    render_close(r);
    return FALSE;
}

static DWORD WINAPI audio_thread(void *arg)
{
    Render r;
    Ring   ring;
    AacDec *dec = NULL;
    IivAudioConfig cfg;
    LONG   mySeq = -1;
    DWORD  taskIdx = 0, lastTry = 0, statTick = GetTickCount();
    HANDLE task, wav = NULL;
    HANDLE evs[2];
    BOOL   playing = FALSE, renderOk = FALSE;
    UINT32 lastSeq = 0;
    int    target = 0, devTarget = 0;
    double avg = 0;                     /* 輪の量の平均(サンプル数) */
    LONG64 arriveSum = 0;
    int    arriveN = 0;
    short *tmp = NULL;
    int    tmpCap = 0;
    (void)arg;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIdx);
    ZeroMemory(&r, sizeof(r));
    ZeroMemory(&ring, sizeof(ring));
    ZeroMemory(&cfg, sizeof(cfg));
    r.ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    evs[0] = g_ev;
    evs[1] = r.ev;

    while (!g_quit) {
        APkt *list;
        LONG  seq;
        DWORD now;
        WaitForMultipleObjects(2, evs, FALSE, 20);
        if (g_quit) break;
        now = GetTickCount();

        /* 形が変わった: 開き直す */
        EnterCriticalSection(&g_cs);
        seq = g_cfgSeq;
        if (seq != mySeq) cfg = g_newCfg;
        LeaveCriticalSection(&g_cs);
        if (seq != mySeq) {
            mySeq = seq;
            aacdec_close(dec);
            dec = NULL;
            if (cfg.codec == IIV_AUDIO_AAC) {
                dec = aacdec_open((int)cfg.rate, 2, cfg.asc);
                if (!dec) log_printf(L"音: AAC を復号できない");
            }
            free(ring.buf);
            ring.cap = (int)cfg.rate;           /* 1 秒 */
            ring.buf = (short *)calloc((size_t)ring.cap, 4);
            ring.rd = ring.n = 0;
            target = (int)cfg.rate * (cfg.codec == IIV_AUDIO_AAC ? TARGET_AAC_MS : TARGET_PCM_MS) / 1000;
            devTarget = (int)cfg.rate * DEV_MS / 1000;
            avg = target;
            playing = FALSE;
            lastSeq = 0;
            renderOk = render_open(&r, (int)cfg.rate);
            lastTry = now;
            if (g_audioDump[0] && !wav) wav = wav_open(g_audioDump, (int)cfg.rate);
        }
        if (!ring.buf) continue;

        /* 1. 届いた音を輪へ */
        EnterCriticalSection(&g_cs);
        list = g_head;
        g_head = g_tail = NULL;
        g_qn = 0;
        LeaveCriticalSection(&g_cs);
        while (list) {
            APkt *k = list;
            const short *pcm = NULL;
            int frames = 0;
            list = k->next;
            g_packets++;
            if (lastSeq && k->h.seq != lastSeq + 1) g_lost += (LONG)(k->h.seq - lastSeq - 1);
            lastSeq = k->h.seq;
            arriveSum += k->recvQpc - k->h.captureQpc;
            arriveN++;
            if (cfg.codec == IIV_AUDIO_PCM) {
                frames = k->len ? k->len / 4 : (int)k->h.frames;
                pcm = k->len ? (const short *)k->data : NULL;
            } else if (dec) {
                frames = aacdec_decode(dec, k->data, k->len, &pcm);
                if (frames < 0) frames = 0;
            }
            if (frames > 0) {
                ring_push(&ring, pcm, frames);
                if (wav) {
                    DWORD wr;
                    if (pcm) WriteFile(wav, pcm, (DWORD)frames * 4, &wr, NULL);
                    else {
                        if (tmpCap < frames) { free(tmp); tmp = (short *)calloc((size_t)frames, 4); tmpCap = tmp ? frames : 0; }
                        if (tmp) { ZeroMemory(tmp, (size_t)frames * 4); WriteFile(wav, tmp, (DWORD)frames * 4, &wr, NULL); }
                    }
                }
            }
            free(k);
        }

        /* 2. デバイスへ。溜まり具合(queued)は、輪とデバイスに入っている分の合計で見る */
        if (!renderOk) {
            if (now - lastTry > 2000) { lastTry = now; renderOk = render_open(&r, (int)cfg.rate); }
            if (!renderOk) { ring_skip(&ring, ring.n > target ? ring.n - target : 0); continue; }
        }
        {
            UINT32 pad = 0;
            HRESULT hr = IAudioClient_GetCurrentPadding(r.ac, &pad);
            int want, n = 0, queued;
            if (FAILED(hr)) {                   /* デバイスが外れたなど: 開き直す */
                log_printf(L"音: デバイスが使えなくなった (0x%08lX)。開き直す", hr);
                render_close(&r);
                renderOk = FALSE;
                lastTry = now - 1500;
                playing = FALSE;
                continue;
            }
            queued = ring.n + (int)pad;
            if (!playing && queued >= target) {
                /* 鳴らし始める。デバイスを開く間などに溜まった分は捨てて、目標の遅れから始める */
                playing = TRUE;
                if (queued > target) ring_skip(&ring, queued - target);
                avg = target;
            } else if (playing && queued > target + (int)cfg.rate * MAX_EXTRA_MS / 1000) {
                /* 溜まりすぎ(通信が詰まった後など): 古い分を捨てて目標へ */
                if (g_drops < 20) log_printf(L"音: 溜まりすぎたので %.0fms 捨てた", (queued - target) * 1000.0 / cfg.rate);
                ring_skip(&ring, queued - target);
                g_drops++;
                avg = target;
            }
            if (!playing) continue;
            want = devTarget - (int)pad;
            if (want > (int)(r.bufFrames - pad)) want = (int)(r.bufFrames - pad);
            if (want <= 0) goto adjust;
            n = want < ring.n ? want : ring.n;
            if (!r.adj && n > 1 && n == want) {
                /* 速さを変えられない: 多ければ 1 サンプル落とし、少なければ 1 サンプル重ねる */
                if (avg > target + cfg.rate / 200 && ring.n > n + 1) ring_skip(&ring, 1);
            }
            if (n > 0) {
                BYTE *p = NULL;
                if (SUCCEEDED(IAudioRenderClient_GetBuffer(r.rc, (UINT32)n, &p))) {
                    if (!r.adj && avg < target - (int)cfg.rate / 200 && n > 1) {
                        ring_pop(&ring, (short *)p, n - 1);                 /* 輪からは 1 つ少なく取り、 */
                        ((DWORD *)p)[n - 1] = ((DWORD *)p)[n - 2];          /* 最後を重ねる */
                    } else {
                        ring_pop(&ring, (short *)p, n);
                    }
                    IAudioRenderClient_ReleaseBuffer(r.rc, (UINT32)n, 0);
                }
            }
            if (ring.n == 0 && pad + (UINT32)n < (UINT32)devTarget / 2) {    /* 途切れた: また溜めてから */
                if (g_underruns < 20) log_printf(L"音: 途切れた(デバイスに残り %.1fms)", (pad + n) * 1000.0 / cfg.rate);
                playing = FALSE;
                g_underruns++;
            }
        adjust:
            /* 時計のずれ: 溜まり具合の平均を目標へ */
            avg += (ring.n + (int)pad + n - avg) * 0.02;
            if (r.adj) {
                double err = (avg - target) / (double)cfg.rate;          /* 秒 */
                double sp = 1.0 + err;                                    /* 1 秒で戻す */
                if (sp > 1 + RATE_ADJ_MAX) sp = 1 + RATE_ADJ_MAX;
                if (sp < 1 - RATE_ADJ_MAX) sp = 1 - RATE_ADJ_MAX;
                if (fabs(sp - r.speed) > 0.0002) {
                    r.speed = sp;
                    IAudioClockAdjustment_SetSampleRate(r.adj, (float)(cfg.rate * sp));
                }
            }
            g_bufMs10 = (LONG)((ring.n + (int)pad + n) * 10000.0 / cfg.rate);
            g_ratePpm = (LONG)((r.speed - 1.0) * 1e6);
        }

        if (now - statTick >= 5000) {
            WCHAR s[256];
            if (arriveN) g_arriveMs10 = (LONG)(arriveSum * 10000.0 / g_qpf / arriveN);
            arriveSum = 0;
            arriveN = 0;
            statTick = now;
            audio_stats(s, ARRAYSIZE(s));
            log_printf(L"%s", s);
        }
    }

    aacdec_close(dec);
    render_close(&r);
    CloseHandle(r.ev);
    free(ring.buf);
    free(tmp);
    wav_close(wav);
    if (task) AvRevertMmThreadCharacteristics(task);
    CoUninitialize();
    {
        WCHAR s[256];
        audio_stats(s, ARRAYSIZE(s));
        log_printf(L"%s(終わり)", s);
    }
    return 0;
}
