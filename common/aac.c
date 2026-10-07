/* ==================================================================
 * aac.c - AAC-LC の符号化と復号(「音を鳴らす」の速度優先)
 *         (iiv-server と iiv-client が共有する。"app.h" はそれぞれの src にある)
 *
 *  Windows に最初からある Microsoft AAC Audio Encoder / Decoder MFT を使う。
 *  中身は raw の AAC(ADTS の見出しなし)。形は AudioSpecificConfig(2 バイト)で伝える。
 *
 *  実測(2026-10-07、tools/audioprobe.c): 48kHz・ステレオ・96kbps で SNR 52dB(スイープと正弦波)。
 *  エンコーダは 3360 サンプル(70ms)入れるまで最初のフレームを出さず、デコーダは 2 フレーム目を
 *  入れてから 1 フレーム目を出す。どちらも低遅延モード(CODECAPI_AVLowLatencyMode)は無い。
 *  サンプルの位置はずれない(先頭の余分はデコーダが落とす)。
 *  デコーダの出力のバッファは GetOutputStreamInfo の大きさ(49152 バイト)が要る。
 *  小さいと、エラーにならずに空のまま返ってくる。
 * ================================================================== */

#include "app.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <wmcodecdsp.h>
#include "aac.h"

#pragma comment(lib, "wmcodecdspuuid.lib")

struct AacEnc {
    IMFTransform  *mft;
    DWORD          outCb;
    unsigned short asc;
    LONG64         pts;
    int            rate;
};

struct AacDec {
    IMFTransform  *mft;
    DWORD          outCb;
    short         *pcm;
    int            pcmCap;          /* サンプル数(1 チャンネルあたり) */
    int            channels;
};

static IMFMediaType *audio_type(const GUID *sub, int rate, int channels)
{
    IMFMediaType *t = NULL;
    if (FAILED(MFCreateMediaType(&t))) return NULL;
    IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, sub);
    IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)rate);
    IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_NUM_CHANNELS, (UINT32)channels);
    return t;
}

static IMFSample *make_sample(const void *data, int len, LONG64 pts, LONG64 dur)
{
    IMFSample      *s = NULL;
    IMFMediaBuffer *b = NULL;
    BYTE           *p;
    if (FAILED(MFCreateMemoryBuffer((DWORD)len, &b))) return NULL;
    if (SUCCEEDED(IMFMediaBuffer_Lock(b, &p, NULL, NULL))) {
        memcpy(p, data, (size_t)len);
        IMFMediaBuffer_Unlock(b);
    }
    IMFMediaBuffer_SetCurrentLength(b, (DWORD)len);
    if (SUCCEEDED(MFCreateSample(&s))) {
        IMFSample_AddBuffer(s, b);
        IMFSample_SetSampleTime(s, pts);
        IMFSample_SetSampleDuration(s, dur);
    }
    IMFMediaBuffer_Release(b);
    return s;
}

/* 出てきたものを 1 つ取る。S_OK、MF_E_TRANSFORM_NEED_MORE_INPUT、失敗のどれか */
static HRESULT take(IMFTransform *mft, DWORD cb, IMFMediaBuffer **out)
{
    MFT_OUTPUT_DATA_BUFFER odb;
    IMFMediaBuffer *b = NULL;
    DWORD st = 0;
    HRESULT hr;
    *out = NULL;
    ZeroMemory(&odb, sizeof(odb));
    if (FAILED(MFCreateSample(&odb.pSample))) return E_OUTOFMEMORY;
    if (FAILED(MFCreateMemoryBuffer(cb, &b))) { IMFSample_Release(odb.pSample); return E_OUTOFMEMORY; }
    IMFSample_AddBuffer(odb.pSample, b);
    IMFMediaBuffer_Release(b);
    hr = IMFTransform_ProcessOutput(mft, 0, 1, &odb, &st);
    if (hr == S_OK) IMFSample_ConvertToContiguousBuffer(odb.pSample, out);
    IMFSample_Release(odb.pSample);
    if (odb.pEvents) IMFCollection_Release(odb.pEvents);
    return hr;
}

/* ------------------------------------------------------------------ */
/*  符号化                                                              */
/* ------------------------------------------------------------------ */

AacEnc *aacenc_open(int rate, int bytesPerSec)
{
    AacEnc *e = (AacEnc *)calloc(1, sizeof(AacEnc));
    IMFMediaType *in = NULL, *out = NULL;
    MFT_OUTPUT_STREAM_INFO si;
    HRESULT hr;
    if (!e) return NULL;
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) { free(e); return NULL; }
    e->rate = rate;
    hr = CoCreateInstance(&CLSID_AACMFTEncoder, NULL, CLSCTX_INPROC_SERVER, &IID_IMFTransform, (void **)&e->mft);
    if (FAILED(hr)) { log_printf(L"音: AAC のエンコーダが無い (0x%08lX)", hr); goto fail; }
    /* このエンコーダは入力の形を先に決める */
    in = audio_type(&MFAudioFormat_PCM, rate, 2);
    out = audio_type(&MFAudioFormat_AAC, rate, 2);
    if (!in || !out) goto fail;
    IMFMediaType_SetUINT32(in, &MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
    IMFMediaType_SetUINT32(in, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32)rate * 4);
    IMFMediaType_SetUINT32(out, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32)bytesPerSec);
    IMFMediaType_SetUINT32(out, &MF_MT_AAC_PAYLOAD_TYPE, 0);                     /* raw */
    IMFMediaType_SetUINT32(out, &MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
    if (FAILED(hr = IMFTransform_SetInputType(e->mft, 0, in, 0)) || FAILED(hr = IMFTransform_SetOutputType(e->mft, 0, out, 0))) {
        log_printf(L"音: AAC のエンコーダの形を決められない (0x%08lX)", hr);
        goto fail;
    }
    IMFMediaType_Release(out);
    out = NULL;
    if (SUCCEEDED(IMFTransform_GetOutputCurrentType(e->mft, 0, &out))) {
        BYTE  *ud = NULL;
        UINT32 n = 0;
        if (SUCCEEDED(IMFMediaType_GetAllocatedBlob(out, &MF_MT_USER_DATA, &ud, &n)) && n >= 14)
            e->asc = (unsigned short)(ud[12] << 8 | ud[13]);           /* HEAACWAVEINFO の後ろの 2 バイト */
        CoTaskMemFree(ud);
    }
    if (!e->asc) goto fail;
    IMFTransform_GetOutputStreamInfo(e->mft, 0, &si);
    e->outCb = si.cbSize > 8192 ? si.cbSize : 8192;
    IMFTransform_ProcessMessage(e->mft, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    IMFMediaType_Release(in);
    IMFMediaType_Release(out);
    return e;
fail:
    if (in) IMFMediaType_Release(in);
    if (out) IMFMediaType_Release(out);
    aacenc_close(e);
    return NULL;
}

unsigned short aacenc_asc(const AacEnc *e) { return e->asc; }

BOOL aacenc_put(AacEnc *e, const short *pcm, int frames, AacOut out, void *ctx)
{
    IMFSample *s;
    HRESULT hr;
    LONG64 dur = (LONG64)frames * 10000000 / e->rate;
    if (!frames) return TRUE;
    s = make_sample(pcm, frames * 4, e->pts, dur);
    if (!s) return FALSE;
    e->pts += dur;
    hr = IMFTransform_ProcessInput(e->mft, 0, s, 0);
    IMFSample_Release(s);
    if (FAILED(hr)) { log_printf(L"音: AAC の符号化 ProcessInput 0x%08lX", hr); return FALSE; }
    for (;;) {
        IMFMediaBuffer *b = NULL;
        hr = take(e->mft, e->outCb, &b);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return TRUE;
        if (FAILED(hr)) { log_printf(L"音: AAC の符号化 ProcessOutput 0x%08lX", hr); return FALSE; }
        if (b) {
            BYTE *p;
            DWORD n = 0;
            if (SUCCEEDED(IMFMediaBuffer_Lock(b, &p, NULL, &n))) {
                if (n) out(ctx, p, (int)n);
                IMFMediaBuffer_Unlock(b);
            }
            IMFMediaBuffer_Release(b);
        }
    }
}

void aacenc_close(AacEnc *e)
{
    if (!e) return;
    if (e->mft) {
        IMFTransform_ProcessMessage(e->mft, MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        IMFTransform_Release(e->mft);
    }
    MFShutdown();
    free(e);
}

/* ------------------------------------------------------------------ */
/*  復号                                                                */
/* ------------------------------------------------------------------ */

AacDec *aacdec_open(int rate, int channels, unsigned short asc)
{
    AacDec *d = (AacDec *)calloc(1, sizeof(AacDec));
    IMFMediaType *in = NULL, *t = NULL;
    MFT_OUTPUT_STREAM_INFO si;
    BYTE ud[14];
    DWORD i;
    HRESULT hr;
    BOOL ok = FALSE;
    if (!d) return NULL;
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) { free(d); return NULL; }
    d->channels = channels;
    hr = CoCreateInstance(&CLSID_CMSAACDecMFT, NULL, CLSCTX_INPROC_SERVER, &IID_IMFTransform, (void **)&d->mft);
    if (FAILED(hr)) { log_printf(L"音: AAC のデコーダが無い (0x%08lX)", hr); goto fail; }
    in = audio_type(&MFAudioFormat_AAC, rate, channels);
    if (!in) goto fail;
    /* MF_MT_USER_DATA = HEAACWAVEINFO の wfx より後ろ(12 バイト)+ AudioSpecificConfig */
    ZeroMemory(ud, sizeof(ud));
    ud[2] = 0x29;                                   /* wAudioProfileLevelIndication */
    ud[12] = (BYTE)(asc >> 8);
    ud[13] = (BYTE)asc;
    IMFMediaType_SetUINT32(in, &MF_MT_AAC_PAYLOAD_TYPE, 0);
    IMFMediaType_SetBlob(in, &MF_MT_USER_DATA, ud, sizeof(ud));
    if (FAILED(hr = IMFTransform_SetInputType(d->mft, 0, in, 0))) { log_printf(L"音: AAC のデコーダの形 (0x%08lX)", hr); goto fail; }
    for (i = 0; SUCCEEDED(IMFTransform_GetOutputAvailableType(d->mft, 0, i, &t)); i++) {
        GUID   sub;
        UINT32 bits = 0;
        IMFMediaType_GetGUID(t, &MF_MT_SUBTYPE, &sub);
        IMFMediaType_GetUINT32(t, &MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
        if (IsEqualGUID(&sub, &MFAudioFormat_PCM) && bits == 16) ok = SUCCEEDED(IMFTransform_SetOutputType(d->mft, 0, t, 0));
        IMFMediaType_Release(t);
        if (ok) break;
    }
    if (!ok) { log_printf(L"音: AAC のデコーダが 16bit で出せない"); goto fail; }
    IMFTransform_GetOutputStreamInfo(d->mft, 0, &si);
    d->outCb = si.cbSize > 8192 ? si.cbSize : 8192;
    IMFTransform_ProcessMessage(d->mft, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    IMFMediaType_Release(in);
    return d;
fail:
    if (in) IMFMediaType_Release(in);
    aacdec_close(d);
    return NULL;
}

int aacdec_decode(AacDec *d, const BYTE *p, int n, const short **pcm)
{
    IMFSample *s = make_sample(p, n, 0, 0);
    HRESULT hr;
    int got = 0;
    *pcm = d->pcm;
    if (!s) return -1;
    hr = IMFTransform_ProcessInput(d->mft, 0, s, 0);
    IMFSample_Release(s);
    if (FAILED(hr)) { log_printf(L"音: AAC の復号 ProcessInput 0x%08lX", hr); return -1; }
    for (;;) {
        IMFMediaBuffer *b = NULL;
        hr = take(d->mft, d->outCb, &b);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
        if (FAILED(hr)) { log_printf(L"音: AAC の復号 ProcessOutput 0x%08lX", hr); return -1; }
        if (b) {
            BYTE *q;
            DWORD len = 0;
            if (SUCCEEDED(IMFMediaBuffer_Lock(b, &q, NULL, &len))) {
                int k = (int)len / (2 * d->channels);
                if (got + k > d->pcmCap) {
                    int cap = (got + k) * 2;
                    short *np = (short *)realloc(d->pcm, (size_t)cap * 2 * d->channels);
                    if (np) { d->pcm = np; d->pcmCap = cap; }
                }
                if (got + k <= d->pcmCap) {
                    memcpy(d->pcm + (size_t)got * d->channels, q, (size_t)k * 2 * d->channels);
                    got += k;
                }
                IMFMediaBuffer_Unlock(b);
            }
            IMFMediaBuffer_Release(b);
        }
    }
    *pcm = d->pcm;
    return got;
}

void aacdec_close(AacDec *d)
{
    if (!d) return;
    if (d->mft) {
        IMFTransform_ProcessMessage(d->mft, MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        IMFTransform_Release(d->mft);
    }
    MFShutdown();
    free(d->pcm);
    free(d);
}
