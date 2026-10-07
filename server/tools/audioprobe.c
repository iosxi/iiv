/* audioprobe.c - 音の部品が使えるかを確かめる(iiv の「音を鳴らす」を作る前の下調べ)
 *
 *   loop    既定の再生デバイスのループバック(鳴っている音の取り込み)の形式と、来かた
 *   render  再生: 自分の形式(16bit ステレオ)のまま渡せるか(AUTOCONVERTPCM)、速さを微調整できるか(RATEADJUST)
 *           音量 0 で無音を流すので、音は出ない
 *   aac     Windows 標準の AAC エンコーダ → デコーダの往復: 遅れ(サンプル数)と SNR
 *
 *   build-tools.bat で build\mf\audioprobe.exe に作る。
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <wmcodecdsp.h>
#include <strmif.h>
#include <initguid.h>
#include <codecapi.h>
#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "oleaut32.lib")
#include <stdio.h>
#include <math.h>

/* C では WASAPI の ID の実体がどのライブラリにも無いので、ここで定義する */
DEFINE_GUID(CLSID_MMDeviceEnumerator, 0xBCDE0395, 0xE52F, 0x467C, 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E);
DEFINE_GUID(IID_IMMDeviceEnumerator, 0xA95664D2, 0x9614, 0x4F35, 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6);
DEFINE_GUID(IID_IAudioClient, 0x1CB9AD4C, 0xDBFA, 0x4C32, 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2);
DEFINE_GUID(IID_IAudioRenderClient, 0xF294ACFC, 0x3146, 0x4483, 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2);
DEFINE_GUID(IID_IAudioCaptureClient, 0xC8ADBD64, 0xE71E, 0x48A0, 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17);
DEFINE_GUID(IID_IAudioClockAdjustment, 0xF6E4C0A0, 0x46D9, 0x4FB8, 0xBE, 0x21, 0x57, 0xA3, 0xEF, 0x2B, 0x62, 0x6C);
DEFINE_GUID(IID_ISimpleAudioVolume, 0x87CE5498, 0x68D6, 0x44E5, 0x92, 0x15, 0x6D, 0xA4, 0x7E, 0xF8, 0x83, 0xD8);
DEFINE_GUID(KSDATAFORMAT_SUBTYPE_PCM, 0x00000001, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71);
DEFINE_GUID(KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, 0x00000003, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71);

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "wmcodecdspuuid.lib")

static LONG64 qpf, qpc0;
static double now_ms(void) { LARGE_INTEGER q; QueryPerformanceCounter(&q); return (q.QuadPart - qpc0) * 1000.0 / qpf; }

static void print_fmt(const char *what, const WAVEFORMATEX *f)
{
    printf("%s: tag=0x%04X ch=%u rate=%lu bits=%u align=%u", what, f->wFormatTag, f->nChannels, f->nSamplesPerSec, f->wBitsPerSample, f->nBlockAlign);
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const WAVEFORMATEXTENSIBLE *e = (const WAVEFORMATEXTENSIBLE *)f;
        printf(" ext: valid=%u mask=0x%lX sub=%s", e->Samples.wValidBitsPerSample, e->dwChannelMask,
               IsEqualGUID(&e->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) ? "float" : IsEqualGUID(&e->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM) ? "pcm" : "?");
    }
    printf("\n");
}

static IMMDevice *default_render(void)
{
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    if (FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **)&en))) return NULL;
    IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    IMMDeviceEnumerator_Release(en);
    return dev;
}

static void pcm16(WAVEFORMATEX *f, DWORD rate)
{
    ZeroMemory(f, sizeof(*f));
    f->wFormatTag = WAVE_FORMAT_PCM;
    f->nChannels = 2;
    f->nSamplesPerSec = rate;
    f->wBitsPerSample = 16;
    f->nBlockAlign = 4;
    f->nAvgBytesPerSec = rate * 4;
}

/* ------------------------------------------------------------------ */

static void loop_try(IMMDevice *dev, const WAVEFORMATEX *fmt, DWORD flags, const char *name)
{
    IAudioClient *ac = NULL;
    IAudioCaptureClient *cc = NULL;
    HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    HRESULT hr;
    int events = 0, packets = 0, silent = 0, timeouts = 0;
    UINT64 frames = 0;
    double t0, maxGap = 0, last;
    if (FAILED(IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&ac))) return;
    hr = IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | flags, 200000, 0, fmt, NULL);
    printf("[loop %s] Initialize 0x%08lX\n", name, hr);
    if (FAILED(hr)) goto done;
    if (flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) {
        hr = IAudioClient_SetEventHandle(ac, ev);
        printf("[loop %s] SetEventHandle 0x%08lX\n", name, hr);
    }
    if (FAILED(IAudioClient_GetService(ac, &IID_IAudioCaptureClient, (void **)&cc))) goto done;
    IAudioClient_Start(ac);
    t0 = last = now_ms();
    while (now_ms() - t0 < 2000) {
        UINT32 n = 0;
        if (flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) {
            if (WaitForSingleObject(ev, 100) == WAIT_OBJECT_0) events++; else timeouts++;
        } else {
            Sleep(10);
        }
        while (SUCCEEDED(IAudioCaptureClient_GetNextPacketSize(cc, &n)) && n) {
            BYTE *p; UINT32 got; DWORD fl;
            if (FAILED(IAudioCaptureClient_GetBuffer(cc, &p, &got, &fl, NULL, NULL))) break;
            packets++;
            frames += got;
            if (fl & AUDCLNT_BUFFERFLAGS_SILENT) silent++;
            IAudioCaptureClient_ReleaseBuffer(cc, got);
            if (now_ms() - last > maxGap) maxGap = now_ms() - last;
            last = now_ms();
        }
    }
    IAudioClient_Stop(ac);
    printf("[loop %s] 2 秒: events %d timeouts %d packets %d (silent %d) frames %llu, パケットの間 最大 %.1fms\n",
           name, events, timeouts, packets, silent, frames, maxGap);
done:
    if (cc) IAudioCaptureClient_Release(cc);
    if (ac) IAudioClient_Release(ac);
    CloseHandle(ev);
}

static void probe_loop(void)
{
    IMMDevice *dev = default_render();
    IAudioClient *ac = NULL;
    WAVEFORMATEX *mix = NULL, f;
    if (!dev) { printf("[loop] 再生デバイスが無い\n"); return; }
    IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&ac);
    IAudioClient_GetMixFormat(ac, &mix);
    print_fmt("[loop] mix", mix);
    IAudioClient_Release(ac);
    loop_try(dev, mix, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, "mix+event");
    loop_try(dev, mix, 0, "mix+poll");
    pcm16(&f, 48000);
    loop_try(dev, &f, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, "s16/48k+autoconvert+poll");
    pcm16(&f, 44100);
    loop_try(dev, &f, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, "s16/44.1k+autoconvert+poll");
    CoTaskMemFree(mix);
    IMMDevice_Release(dev);
}

/* ------------------------------------------------------------------ */

static void render_try(DWORD rate, DWORD flags, const char *name)
{
    IMMDevice *dev = default_render();
    IAudioClient *ac = NULL;
    IAudioRenderClient *rc = NULL;
    IAudioClockAdjustment *adj = NULL;
    ISimpleAudioVolume *vol = NULL;
    HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    WAVEFORMATEX f;
    UINT32 buf = 0;
    REFERENCE_TIME def = 0, mn = 0, lat = 0;
    HRESULT hr;
    int events = 0;
    double t0;
    pcm16(&f, rate);
    if (!dev || FAILED(IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&ac))) return;
    hr = IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK | flags, 300000, 0, &f, NULL);
    printf("[render %s %lu] Initialize 0x%08lX\n", name, rate, hr);
    if (FAILED(hr)) goto done;
    IAudioClient_SetEventHandle(ac, ev);
    IAudioClient_GetBufferSize(ac, &buf);
    IAudioClient_GetDevicePeriod(ac, &def, &mn);
    IAudioClient_GetStreamLatency(ac, &lat);
    printf("[render %s %lu] buffer %u frames (%.1fms) period %.1f/%.1fms latency %.1fms\n", name, rate, buf, buf * 1000.0 / rate,
           def / 10000.0, mn / 10000.0, lat / 10000.0);
    if (SUCCEEDED(IAudioClient_GetService(ac, &IID_ISimpleAudioVolume, (void **)&vol))) ISimpleAudioVolume_SetMasterVolume(vol, 0.0f, NULL);
    hr = IAudioClient_GetService(ac, &IID_IAudioClockAdjustment, (void **)&adj);
    printf("[render %s %lu] IAudioClockAdjustment 0x%08lX", name, rate, hr);
    if (adj) printf(" SetSampleRate(+0.5%%) 0x%08lX", IAudioClockAdjustment_SetSampleRate(adj, rate * 1.005f));
    printf("\n");
    IAudioClient_GetService(ac, &IID_IAudioRenderClient, (void **)&rc);
    IAudioClient_Start(ac);
    t0 = now_ms();
    {
        UINT64 written = 0;
        while (now_ms() - t0 < 1000) {
            UINT32 pad = 0, n;
            BYTE *p;
            if (WaitForSingleObject(ev, 200) == WAIT_OBJECT_0) events++;
            IAudioClient_GetCurrentPadding(ac, &pad);
            n = buf - pad;
            if (n && SUCCEEDED(IAudioRenderClient_GetBuffer(rc, n, &p))) {
                IAudioRenderClient_ReleaseBuffer(rc, n, AUDCLNT_BUFFERFLAGS_SILENT);
                written += n;
            }
        }
        printf("[render %s %lu] 1 秒: events %d、書いた %llu frames(+0.5%% にしたなら 1 秒で約 %.0f 減るはず)\n", name, rate, events, written,
               rate * 1.005);
    }
    IAudioClient_Stop(ac);
done:
    if (rc) IAudioRenderClient_Release(rc);
    if (adj) IAudioClockAdjustment_Release(adj);
    if (vol) ISimpleAudioVolume_Release(vol);
    if (ac) IAudioClient_Release(ac);
    if (dev) IMMDevice_Release(dev);
    CloseHandle(ev);
}

static void probe_render(void)
{
    render_try(48000, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, "autoconvert");
    render_try(44100, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, "autoconvert");
    render_try(48000, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY | AUDCLNT_STREAMFLAGS_RATEADJUST, "autoconvert+rateadjust");
    render_try(48000, AUDCLNT_STREAMFLAGS_RATEADJUST, "rateadjust(変換なし)");
}

/* ------------------------------------------------------------------ */

static IMFTransform *make_mft(const CLSID *clsid)
{
    IMFTransform *t = NULL;
    HRESULT hr = CoCreateInstance(clsid, NULL, CLSCTX_INPROC_SERVER, &IID_IMFTransform, (void **)&t);
    if (FAILED(hr)) printf("[aac] CoCreateInstance 0x%08lX\n", hr);
    return t;
}

static IMFMediaType *audio_type(const GUID *sub, DWORD rate)
{
    IMFMediaType *t = NULL;
    MFCreateMediaType(&t);
    IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, sub);
    IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
    IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_NUM_CHANNELS, 2);
    return t;
}

static int g_pk[4096], g_npk;

static int drain(IMFTransform *t, DWORD cb, BYTE **out, int *outLen, int *outCap, int *packets, int *packetBytes)
{
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER odb;
        DWORD st = 0;
        IMFMediaBuffer *b = NULL;
        HRESULT hr;
        ZeroMemory(&odb, sizeof(odb));
        MFCreateSample(&odb.pSample);
        MFCreateMemoryBuffer(cb ? cb : 65536, &b);
        IMFSample_AddBuffer(odb.pSample, b);
        IMFMediaBuffer_Release(b);
        hr = IMFTransform_ProcessOutput(t, 0, 1, &odb, &st);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) { IMFSample_Release(odb.pSample); return 0; }
        if (FAILED(hr)) { printf("[aac] ProcessOutput 0x%08lX\n", hr); IMFSample_Release(odb.pSample); return -1; }
        {
            IMFMediaBuffer *cbuf = NULL;
            BYTE *p; DWORD len = 0;
            IMFSample_ConvertToContiguousBuffer(odb.pSample, &cbuf);
            IMFMediaBuffer_Lock(cbuf, &p, NULL, &len);
            if (*outLen + (int)len > *outCap) { *outCap = (*outLen + (int)len) * 2; *out = (BYTE *)realloc(*out, (size_t)*outCap); }
            memcpy(*out + *outLen, p, len);
            *outLen += (int)len;
            if (packets) { if (g_npk < 4096) g_pk[g_npk++] = (int)len; packets[0]++; packetBytes[0] += (int)len; }
            IMFMediaBuffer_Unlock(cbuf);
            IMFMediaBuffer_Release(cbuf);
        }
        IMFSample_Release(odb.pSample);
    }
}

static void probe_aac(DWORD rate, DWORD bytesPerSec)
{
    IMFTransform *enc = make_mft(&CLSID_AACMFTEncoder), *dec = make_mft(&CLSID_CMSAACDecMFT);
    IMFMediaType *in, *out;
    HRESULT hr;
    const int N = rate * 2;                 /* 2 秒 */
    short *pcm = (short *)malloc((size_t)N * 4);
    BYTE *user = NULL; UINT32 userLen = 0;
    int i;
    BYTE *aac = NULL; int aacLen = 0, aacCap = 0, packets = 0, packetBytes = 0;
    int firstPacketAt = -1;
    double encMs = 0, decMs = 0;
    if (!enc || !dec || !pcm) return;
    g_npk = 0;
    for (i = 0; i < N; i++) {               /* 1kHz と 5kHz を混ぜた音(左右で少し違う) */
        double t = (double)i / rate;
        /* 左: 2 秒で 200Hz → 8kHz のスイープ(周期が無いので、遅れを相関で一意に決められる) */
        pcm[i * 2] = (short)(9000 * sin(2 * 3.14159265 * (200 * t + (8000 - 200) * t * t / 4)));
        pcm[i * 2 + 1] = (short)(9000 * sin(2 * 3.14159265 * 1500 * t));
    }
    if (getenv("LOWLAT")) {             /* 低遅延モードを頼んでみる */
        ICodecAPI *api = NULL;
        int w;
        for (w = 0; w < 2; w++) {
            IMFTransform *t = w ? dec : enc;
            if (SUCCEEDED(IMFTransform_QueryInterface(t, &IID_ICodecAPI, (void **)&api))) {
                VARIANT v;
                VariantInit(&v);
                v.vt = VT_UI4;
                v.ulVal = 1;
                printf("[aac] %s CODECAPI_AVLowLatencyMode 0x%08lX\n", w ? "dec" : "enc", ICodecAPI_SetValue(api, &CODECAPI_AVLowLatencyMode, &v));
                ICodecAPI_Release(api);
            } else printf("[aac] %s ICodecAPI なし\n", w ? "dec" : "enc");
        }
    }
    /* エンコーダ: 入力 → 出力の順 */
    in = audio_type(&MFAudioFormat_PCM, rate);
    IMFMediaType_SetUINT32(in, &MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
    IMFMediaType_SetUINT32(in, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * 4);
    hr = IMFTransform_SetInputType(enc, 0, in, 0);
    printf("[aac %lu %lukbps] enc SetInputType 0x%08lX\n", rate, bytesPerSec * 8 / 1000, hr);
    out = audio_type(&MFAudioFormat_AAC, rate);
    IMFMediaType_SetUINT32(out, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bytesPerSec);
    IMFMediaType_SetUINT32(out, &MF_MT_AAC_PAYLOAD_TYPE, 0);
    IMFMediaType_SetUINT32(out, &MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
    hr = IMFTransform_SetOutputType(enc, 0, out, 0);
    printf("[aac] enc SetOutputType 0x%08lX\n", hr);
    if (FAILED(hr)) return;
    IMFMediaType_Release(out);
    IMFTransform_GetOutputCurrentType(enc, 0, &out);
    IMFMediaType_GetAllocatedBlob(out, &MF_MT_USER_DATA, &user, &userLen);
    printf("[aac] user data %u バイト:", userLen);
    for (i = 0; i < (int)userLen; i++) printf(" %02X", user[i]);
    printf("\n");
    /* 10ms ずつ入れる(取り込みと同じ刻み) */
    {
        int chunk = rate / 100, pos;
        LONG64 pts = 0;
        for (pos = 0; pos + chunk <= N; pos += chunk) {
            IMFSample *s; IMFMediaBuffer *b; BYTE *p;
            double t0 = now_ms();
            int before = packets;
            MFCreateMemoryBuffer(chunk * 4, &b);
            IMFMediaBuffer_Lock(b, &p, NULL, NULL);
            memcpy(p, pcm + pos * 2, (size_t)chunk * 4);
            IMFMediaBuffer_Unlock(b);
            IMFMediaBuffer_SetCurrentLength(b, chunk * 4);
            MFCreateSample(&s);
            IMFSample_AddBuffer(s, b);
            IMFSample_SetSampleTime(s, pts);
            IMFSample_SetSampleDuration(s, 100000);
            pts += 100000;
            hr = IMFTransform_ProcessInput(enc, 0, s, 0);
            if (FAILED(hr)) printf("[aac] enc ProcessInput 0x%08lX\n", hr);
            IMFSample_Release(s); IMFMediaBuffer_Release(b);
            drain(enc, 0, &aac, &aacLen, &aacCap, &packets, &packetBytes);
            encMs += now_ms() - t0;
            if (firstPacketAt < 0 && packets > before) firstPacketAt = pos + chunk;
        }
    }
    printf("[aac] 2 秒を 10ms ずつ: AAC のパケット %d 個 %d バイト(%.0f kbps)、最初のパケットが出たのは %d サンプル入れた後、符号化 合計 %.1fms\n",
           packets, packetBytes, packetBytes * 8 / 2.0 / 1000, firstPacketAt, encMs);
    /* デコーダ */
    {
        IMFMediaType *dt = audio_type(&MFAudioFormat_AAC, rate);
        BYTE *pcmOut = NULL; int outLen = 0, outCap = 0;
        int off = 0, k, decFirst = 0;
        IMFMediaType_SetUINT32(dt, &MF_MT_AAC_PAYLOAD_TYPE, 0);
        IMFMediaType_SetBlob(dt, &MF_MT_USER_DATA, user, userLen);
        hr = IMFTransform_SetInputType(dec, 0, dt, 0);
        printf("[aac] dec SetInputType 0x%08lX\n", hr);
        for (k = 0; ; k++) {
            IMFMediaType *t = NULL; GUID sub;
            if (FAILED(IMFTransform_GetOutputAvailableType(dec, 0, (DWORD)k, &t))) break;
            IMFMediaType_GetGUID(t, &MF_MT_SUBTYPE, &sub);
            if (IsEqualGUID(&sub, &MFAudioFormat_PCM)) { hr = IMFTransform_SetOutputType(dec, 0, t, 0); IMFMediaType_Release(t); break; }
            IMFMediaType_Release(t);
        }
        printf("[aac] dec SetOutputType(PCM) 0x%08lX\n", hr);
        /* 1 パケットずつ(エンコーダが出した区切りのまま)入れる */
        IMFTransform_ProcessMessage(dec, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        for (k = 0; k < g_npk; k++) {
            IMFSample *s; IMFMediaBuffer *b; BYTE *p;
            int pk = 0, pb = 0;
            double t0 = now_ms();
            MFCreateMemoryBuffer((DWORD)g_pk[k], &b);
            IMFMediaBuffer_Lock(b, &p, NULL, NULL);
            memcpy(p, aac + off, (size_t)g_pk[k]);
            IMFMediaBuffer_Unlock(b);
            IMFMediaBuffer_SetCurrentLength(b, (DWORD)g_pk[k]);
            off += g_pk[k];
            MFCreateSample(&s);
            IMFSample_AddBuffer(s, b);
            IMFSample_SetSampleTime(s, (LONG64)k * 1024 * 10000000 / rate);
            IMFSample_SetSampleDuration(s, (LONG64)1024 * 10000000 / rate);
            hr = IMFTransform_ProcessInput(dec, 0, s, 0);
            if (FAILED(hr) || k < 3) printf("[aac] dec ProcessInput[%d] %d バイト 0x%08lX\n", k, g_pk[k], hr);
            if (k < 3) {
                DWORD fl = 0;
                MFT_OUTPUT_STREAM_INFO si;
                IMFTransform_GetOutputStatus(dec, &fl);
                IMFTransform_GetOutputStreamInfo(dec, 0, &si);
                printf("[aac] dec OutputStatus 0x%lX streaminfo flags 0x%lX cb %lu\n", fl, si.dwFlags, si.cbSize);
            }
            IMFSample_Release(s); IMFMediaBuffer_Release(b);
            drain(dec, 49152, &pcmOut, &outLen, &outCap, NULL, NULL);
            decMs += now_ms() - t0;
            if (outLen && !decFirst) { decFirst = k + 1; printf("[aac] デコーダが最初に出したのは %d パケット入れた後\n", decFirst); }
            (void)pk; (void)pb;
        }
        /* 遅れ: 相関が最大になるずれ(0〜4096 サンプル) */
        {
            int outN = outLen / 4, best = 0, d;
            const short *o = (const short *)pcmOut;
            double bestC = -1e300, sig = 0, err = 0;
            for (d = 0; d < 8192 && d < outN; d++) {
                double c = 0; int j;
                for (j = rate / 2; j < rate / 2 + 4800 && j + d < outN; j++) c += (double)pcm[j * 2] * o[(j + d) * 2];
                if (c > bestC) { bestC = c; best = d; }
            }
            for (i = rate / 2; i < N - 8192 && i + best < outN; i++) {
                double a = pcm[i * 2], b2 = o[(i + best) * 2], a2 = pcm[i * 2 + 1], b3 = o[(i + best) * 2 + 1];
                sig += a * a + a2 * a2;
                err += (a - b2) * (a - b2) + (a2 - b3) * (a2 - b3);
            }
            printf("[aac] 復号: %d サンプル出た、遅れ %d サンプル(%.1fms)、SNR %.1fdB、復号 合計 %.1fms\n",
                   outN, best, best * 1000.0 / rate, 10 * log10(sig / (err > 0 ? err : 1)), decMs);
        }
        free(pcmOut);
        IMFMediaType_Release(dt);
    }
    CoTaskMemFree(user);
    IMFMediaType_Release(in);
    IMFMediaType_Release(out);
    IMFTransform_Release(enc);
    IMFTransform_Release(dec);
    free(aac);
    free(pcm);
}

int main(int argc, char **argv)
{
    LARGE_INTEGER q;
    const char *what = argc > 1 ? argv[1] : "all";
    SetConsoleOutputCP(65001);
    QueryPerformanceFrequency(&q); qpf = q.QuadPart;
    QueryPerformanceCounter(&q); qpc0 = q.QuadPart;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (!strcmp(what, "all") || !strcmp(what, "loop")) probe_loop();
    if (!strcmp(what, "all") || !strcmp(what, "render")) probe_render();
    if (!strcmp(what, "all") || !strcmp(what, "aac")) {
        probe_aac(48000, 16000);
        probe_aac(48000, 12000);
    }
    MFShutdown();
    CoUninitialize();
    return 0;
}
