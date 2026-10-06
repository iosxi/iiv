/* ==================================================================
 * vdec.c - H.264 の復号
 *
 *  Windows に最初からある H.264 のデコーダ(Microsoft H264 Video Decoder MFT)を
 *  低遅延モードで使う(1 枚入れると 1 枚出る)。
 *   GPU  表示の窓の D3D11 のデバイスを渡す(DXVA)。出てくる絵は GPU のテクスチャ。
 *        view_submit_nv12 で窓へ渡し、窓がシェーダーで RGB にして描く(CPU に降ろさない)。
 *   CPU  GDI で描くとき・GPU の復号ができないとき。出てきた NV12 を BT.709(16〜235)の
 *        式で BGRX に戻し、画面の写し(g_rm.fb)に書く。色の変換は SSE2 で 8 画素ずつ。
 *  サーバーは BT.709・16〜235 で符号化する(iiv-server の video.c)。
 * ================================================================== */

#include "iivc.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <strmif.h>
#include <emmintrin.h>
#include <d3d11.h>
#include <initguid.h>
#include <codecapi.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "strmiids.lib")

static IMFTransform *g_mft;
static UINT32        g_ow, g_oh;        /* 出力の形(高さは 16 の倍数に切り上げられていることがある) */
static BOOL          g_provides;
static DWORD         g_outCb;
static int           g_w, g_h;
static BOOL          g_mfUp;
static WCHAR         g_name[96];
static LONG64        g_pts;
static IMFDXGIDeviceManager *g_mgr;     /* GPU で復号するとき */
static BOOL          g_gpu;

/* ------------------------------------------------------------------ */
/*  NV12 → BGRX(BT.709、16〜235)                                       */
/* ------------------------------------------------------------------ */

/* R = 1.164(Y-16) + 1.793(V-128)
   G = 1.164(Y-16) - 0.213(U-128) - 0.533(V-128)
   B = 1.164(Y-16) + 2.112(U-128)
   係数は 1024 倍、値は 64 倍して _mm_mulhi_epi16 で (a*b)>>16 = 元の値 × 係数 */
#define K_Y   1192
#define K_RV  1836
#define K_GU  (-218)
#define K_GV  (-546)
#define K_BU  2163

static __forceinline void conv8(const BYTE *y, const BYTE *uv, BYTE *out)
{
    const __m128i zero = _mm_setzero_si128(), c16 = _mm_set1_epi16(16), c128 = _mm_set1_epi16(128);
    __m128i yy = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)y), zero);
    __m128i x  = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)uv), zero);     /* U0 V0 U1 V1 U2 V2 U3 V3 */
    __m128i u  = _mm_shufflehi_epi16(_mm_shufflelo_epi16(x, _MM_SHUFFLE(2, 2, 0, 0)), _MM_SHUFFLE(2, 2, 0, 0));
    __m128i v  = _mm_shufflehi_epi16(_mm_shufflelo_epi16(x, _MM_SHUFFLE(3, 3, 1, 1)), _MM_SHUFFLE(3, 3, 1, 1));
    __m128i ys, us, vs, r, g, b, bg, ra;
    ys = _mm_mulhi_epi16(_mm_slli_epi16(_mm_sub_epi16(yy, c16), 6), _mm_set1_epi16(K_Y));
    us = _mm_slli_epi16(_mm_sub_epi16(u, c128), 6);
    vs = _mm_slli_epi16(_mm_sub_epi16(v, c128), 6);
    r = _mm_add_epi16(ys, _mm_mulhi_epi16(vs, _mm_set1_epi16(K_RV)));
    g = _mm_add_epi16(ys, _mm_add_epi16(_mm_mulhi_epi16(us, _mm_set1_epi16(K_GU)), _mm_mulhi_epi16(vs, _mm_set1_epi16(K_GV))));
    b = _mm_add_epi16(ys, _mm_mulhi_epi16(us, _mm_set1_epi16(K_BU)));
    r = _mm_packus_epi16(r, r);
    g = _mm_packus_epi16(g, g);
    b = _mm_packus_epi16(b, b);
    bg = _mm_unpacklo_epi8(b, g);
    ra = _mm_unpacklo_epi8(r, _mm_set1_epi8(-1));
    _mm_storeu_si128((__m128i *)out, _mm_unpacklo_epi16(bg, ra));
    _mm_storeu_si128((__m128i *)(out + 16), _mm_unpackhi_epi16(bg, ra));
}

static void conv1(int Y, int U, int V, BYTE *out)
{
    int c = (Y - 16) * 1192, d = U - 128, e = V - 128;
    int r = (c + 1836 * e) >> 10, g = (c - 218 * d - 546 * e) >> 10, b = (c + 2163 * d) >> 10;
    out[0] = (BYTE)(b < 0 ? 0 : b > 255 ? 255 : b);
    out[1] = (BYTE)(g < 0 ? 0 : g > 255 ? 255 : g);
    out[2] = (BYTE)(r < 0 ? 0 : r > 255 ? 255 : r);
    out[3] = 255;
}

static void nv12_to_bgrx(const BYTE *src, LONG pitch, UINT32 planeH, BYTE *dst, int w, int h)
{
    const BYTE *uvp = src + (size_t)pitch * planeH;
    int x, y;
    for (y = 0; y < h; y++) {
        const BYTE *yr = src + (size_t)y * pitch, *uvr = uvp + (size_t)(y / 2) * pitch;
        BYTE *o = dst + (size_t)y * w * 4;
        for (x = 0; x + 8 <= w; x += 8) conv8(yr + x, uvr + x, o + (size_t)x * 4);
        for (; x < w; x++) conv1(yr[x], uvr[x & ~1], uvr[(x & ~1) + 1], o + (size_t)x * 4);
    }
}

/* ------------------------------------------------------------------ */
/*  デコーダ                                                            */
/* ------------------------------------------------------------------ */

static BOOL set_out_type(void)
{
    IMFMediaType *t = NULL;
    DWORD i;
    MFT_OUTPUT_STREAM_INFO si;
    BOOL ok = FALSE;
    for (i = 0; SUCCEEDED(IMFTransform_GetOutputAvailableType(g_mft, 0, i, &t)); i++) {
        GUID sub;
        IMFMediaType_GetGUID(t, &MF_MT_SUBTYPE, &sub);
        if (IsEqualGUID(&sub, &MFVideoFormat_NV12)) {
            UINT64 fs = 0;
            if (SUCCEEDED(IMFTransform_SetOutputType(g_mft, 0, t, 0))) {
                IMFMediaType_GetUINT64(t, &MF_MT_FRAME_SIZE, &fs);
                g_ow = (UINT32)(fs >> 32);
                g_oh = (UINT32)fs;
                ok = TRUE;
            }
            IMFMediaType_Release(t);
            break;
        }
        IMFMediaType_Release(t);
    }
    IMFTransform_GetOutputStreamInfo(g_mft, 0, &si);
    g_provides = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    g_outCb = si.cbSize;
    return ok;
}

void vdec_close(void)
{
    if (g_mft) {
        IMFTransform_ProcessMessage(g_mft, MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        IMFTransform_Release(g_mft);
        g_mft = NULL;
    }
    if (g_mgr) { IMFDXGIDeviceManager_Release(g_mgr); g_mgr = NULL; }
    g_gpu = FALSE;
}

BOOL vdec_is_gpu(void) { return g_gpu; }

BOOL vdec_open(int w, int h, void *d3dDevice)
{
    MFT_REGISTER_TYPE_INFO in = { MFMediaType_Video, MFVideoFormat_H264 };
    IMFActivate **act = NULL;
    UINT32 n = 0, i, len;
    IMFMediaType *mt = NULL;
    ICodecAPI *api = NULL;
    WCHAR *fn = NULL;
    HRESULT hr;

    vdec_close();
    if (!g_mfUp) {
        CoInitializeEx(NULL, COINIT_MULTITHREADED);
        if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return FALSE;
        g_mfUp = TRUE;
    }
    if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, &in, NULL, &act, &n)) || !n) {
        log_printf(L"H.264 のデコーダが無い");
        return FALSE;
    }
    IMFActivate_GetAllocatedString(act[0], &MFT_FRIENDLY_NAME_Attribute, &fn, &len);
    lstrcpynW(g_name, fn ? fn : L"?", ARRAYSIZE(g_name));
    CoTaskMemFree(fn);
    hr = IMFActivate_ActivateObject(act[0], &IID_IMFTransform, (void **)&g_mft);
    for (i = 0; i < n; i++) IMFActivate_Release(act[i]);
    CoTaskMemFree(act);
    if (FAILED(hr)) return FALSE;
    if (d3dDevice) {
        UINT token = 0;
        if (SUCCEEDED(MFCreateDXGIDeviceManager(&token, &g_mgr)) &&
            SUCCEEDED(IMFDXGIDeviceManager_ResetDevice(g_mgr, (IUnknown *)d3dDevice, token)) &&
            SUCCEEDED(IMFTransform_ProcessMessage(g_mft, MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)g_mgr))) {
            g_gpu = TRUE;
        } else {
            log_printf(L"復号: GPU が使えないので CPU で");
            if (g_mgr) { IMFDXGIDeviceManager_Release(g_mgr); g_mgr = NULL; }
        }
    }
    if (SUCCEEDED(IMFTransform_QueryInterface(g_mft, &IID_ICodecAPI, (void **)&api))) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_UI4;
        v.ulVal = 1;
        ICodecAPI_SetValue(api, &CODECAPI_AVLowLatencyMode, &v);
        ICodecAPI_Release(api);
    }
    if (FAILED(MFCreateMediaType(&mt))) { vdec_close(); return FALSE; }
    IMFMediaType_SetGUID(mt, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(mt, &MF_MT_SUBTYPE, &MFVideoFormat_H264);
    IMFMediaType_SetUINT64(mt, &MF_MT_FRAME_SIZE, ((UINT64)w << 32) | (UINT32)h);
    IMFMediaType_SetUINT64(mt, &MF_MT_FRAME_RATE, ((UINT64)60 << 32) | 1);
    IMFMediaType_SetUINT32(mt, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    hr = IMFTransform_SetInputType(g_mft, 0, mt, 0);
    IMFMediaType_Release(mt);
    if (FAILED(hr) || !set_out_type()) { log_printf(L"デコーダの形を決められない (0x%08lX)", hr); vdec_close(); return FALSE; }
    IMFTransform_ProcessMessage(g_mft, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    IMFTransform_ProcessMessage(g_mft, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    g_w = w;
    g_h = h;
    g_pts = 0;
    log_printf(L"復号: %s(%s)%dx%d", g_name, g_gpu ? L"GPU" : L"CPU", w, h);
    return TRUE;
}

const WCHAR *vdec_name(void) { return g_name; }

/* GPU: 出てきたテクスチャを窓へ渡す */
static void submit_frame(IMFSample *s)
{
    IMFMediaBuffer  *b = NULL;
    IMFDXGIBuffer   *db = NULL;
    ID3D11Texture2D *tex = NULL;
    UINT sub = 0;
    if (FAILED(IMFSample_GetBufferByIndex(s, 0, &b))) return;
    if (SUCCEEDED(IMFMediaBuffer_QueryInterface(b, &IID_IMFDXGIBuffer, (void **)&db)) &&
        SUCCEEDED(IMFDXGIBuffer_GetResource(db, &IID_ID3D11Texture2D, (void **)&tex))) {
        IMFDXGIBuffer_GetSubresourceIndex(db, &sub);
        if (view_submit_nv12(tex, sub, (int)g_ow, (int)g_oh)) {
            AcquireSRWLockExclusive(&g_rm.lock);
            SetRect(&g_rm.dirty, 0, 0, g_w, g_h);
            ReleaseSRWLockExclusive(&g_rm.lock);
        }
        ID3D11Texture2D_Release(tex);
    }
    if (db) IMFDXGIBuffer_Release(db);
    IMFMediaBuffer_Release(b);
}

/* CPU: 出てきたフレームを fb に書く */
static void write_frame(IMFSample *s)
{
    IMFMediaBuffer *b = NULL;
    IMF2DBuffer    *b2 = NULL;
    BYTE *p = NULL;
    LONG  pitch = (LONG)g_ow;
    DWORD len = 0;
    BOOL  locked2d = FALSE;
    if (FAILED(IMFSample_ConvertToContiguousBuffer(s, &b))) return;
    if (SUCCEEDED(IMFMediaBuffer_QueryInterface(b, &IID_IMF2DBuffer, (void **)&b2)) && SUCCEEDED(IMF2DBuffer_Lock2D(b2, &p, &pitch))) locked2d = TRUE;
    else if (FAILED(IMFMediaBuffer_Lock(b, &p, NULL, &len))) p = NULL;
    if (p) {
        AcquireSRWLockExclusive(&g_rm.lock);
        if (g_rm.fb && g_rm.w == g_w && g_rm.h == g_h && (int)g_ow >= g_w && (int)g_oh >= g_h) {
            nv12_to_bgrx(p, pitch, g_oh, g_rm.fb, g_w, g_h);
            SetRect(&g_rm.dirty, 0, 0, g_w, g_h);
        }
        ReleaseSRWLockExclusive(&g_rm.lock);
        if (locked2d) IMF2DBuffer_Unlock2D(b2); else IMFMediaBuffer_Unlock(b);
    }
    if (b2) IMF2DBuffer_Release(b2);
    IMFMediaBuffer_Release(b);
}

BOOL vdec_decode(const BYTE *data, int len, BOOL *gotFrame)
{
    IMFSample *s = NULL;
    IMFMediaBuffer *b = NULL;
    BYTE *p;
    HRESULT hr;

    *gotFrame = FALSE;
    if (!g_mft) return FALSE;
    if (FAILED(MFCreateMemoryBuffer((DWORD)len, &b))) return FALSE;
    IMFMediaBuffer_Lock(b, &p, NULL, NULL);
    memcpy(p, data, (size_t)len);
    IMFMediaBuffer_Unlock(b);
    IMFMediaBuffer_SetCurrentLength(b, (DWORD)len);
    MFCreateSample(&s);
    IMFSample_AddBuffer(s, b);
    IMFMediaBuffer_Release(b);
    IMFSample_SetSampleTime(s, g_pts);
    IMFSample_SetSampleDuration(s, 166667);
    g_pts += 166667;
    hr = IMFTransform_ProcessInput(g_mft, 0, s, 0);
    IMFSample_Release(s);
    if (FAILED(hr)) { log_printf(L"復号: ProcessInput 0x%08lX", hr); return FALSE; }
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER odb;
        DWORD st = 0;
        ZeroMemory(&odb, sizeof(odb));
        if (!g_provides) {
            IMFMediaBuffer *ob;
            if (FAILED(MFCreateSample(&odb.pSample))) return FALSE;
            if (FAILED(MFCreateMemoryBuffer(g_outCb ? g_outCb : (DWORD)(g_ow * g_oh * 3 / 2), &ob))) { IMFSample_Release(odb.pSample); return FALSE; }
            IMFSample_AddBuffer(odb.pSample, ob);
            IMFMediaBuffer_Release(ob);
        }
        hr = IMFTransform_ProcessOutput(g_mft, 0, 1, &odb, &st);
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (odb.pSample) IMFSample_Release(odb.pSample);
            if (!set_out_type()) return FALSE;
            continue;
        }
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            if (odb.pSample) IMFSample_Release(odb.pSample);
            break;
        }
        if (FAILED(hr)) {
            if (odb.pSample) IMFSample_Release(odb.pSample);
            log_printf(L"復号: ProcessOutput 0x%08lX", hr);
            return FALSE;
        }
        if (odb.pSample) {
            if (g_gpu) submit_frame(odb.pSample); else write_frame(odb.pSample);
            *gotFrame = TRUE;
            IMFSample_Release(odb.pSample);
        }
        if (odb.pEvents) IMFCollection_Release(odb.pEvents);
    }
    return TRUE;
}
