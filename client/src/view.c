/* ==================================================================
 * view.c - 相手の画面を出す窓
 *
 *  描画は D3D11(フリップ モデルのスワップ チェーン)。変わった範囲だけを
 *  GPU のテクスチャへ写し、拡大縮小は GPU で行う。写すときは自前の
 *  1024×1024 の STAGING テクスチャ 1 枚を経由し、大きな範囲はタイルに分けて送る
 *  (UpdateSubresource はドライバが送り出し用の領域を毎回用意し、絵が途切れなく
 *  届くと 4K で 200MB 近くまで溜まった。2026-10-04 実測)。縮めるときはミップ
 *  マップを作って三線形で引く。D3D11 が使えなければ GDI(StretchDIBits)。
 *  既定は GDI(メモリが少ない)。接続の画面・メニュー・ini の render=gpu で D3D11 にでき、
 *  つないだまま切り替えられる。D3D11 のスワップチェーンは窓に重ねた子窓(キャンバス)に
 *  結びつけ、GDI へ戻すときは子窓ごと捨てる(flip モデルを結びつけた窓は、スワップチェーンを
 *  捨てても最後の絵が出たままになり、GDI で描いても映らないため)。子窓は無効にしてあるので、
 *  マウスは親の窓が受け取る。
 *
 *  入力
 *   - キーはスキャン コード(拡張 = E0 付き)と仮想キーをそのまま送る(相手も Windows)。
 *     IME はこの窓では切る(打った文字は相手の IME で変換する)。
 *   - 全画面のとき(ini の grab=always なら いつも)は低レベルのキーボード
 *     フックで Windows キーや Alt+Tab も相手へ送る。
 *   - Ctrl+Alt+Enter で全画面の切り替え、F8 でメニュー(どちらも相手へは送らない)。
 *   - 全画面のときに画面の上端へマウスを当てると、帯(窓に戻す・メニュー・最小化・切断)が出る。
 *     キーを横取りしている間でも、マウスで必ず抜けられるようにするため。
 *   - 窓が手前でなくなったら、押したままのキー・ボタンを離したことにする。
 *
 *  カーソルは相手から形をもらい、こちらで描く(表示の倍率に合わせて縮める)。
 * ================================================================== */

#include "iivc.h"
#include "resource.h"
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_5.h>
#include <imm.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include "shader_vs.h"
#include "shader_ps.h"
#include "shader_nv12.h"

#define VIEW_CLASS   L"iiv.Client.View"
#define TIMER_STATS  1
#define TIMER_IDLE   2
#define TIMER_BAR    3                  /* 上端に当て続けたら帯を出す */
#define TIMER_BARHIDE 4                 /* 帯から離れたら消す */
#define BAR_CLASS    L"iiv.Client.Bar"

enum { IDB_RESTORE = 0x180, IDB_MENU, IDB_MIN, IDB_CLOSE, IDB_STATS };

enum {
    IDM_FULLSCREEN = 0x100, IDM_FIT, IDM_ACTUAL, IDM_Q_HIGH, IDM_Q_LOSSLESS, IDM_Q_NORMAL, IDM_Q_LOW,
    IDM_VIEWONLY, IDM_CAD, IDM_SENDF8, IDM_REFRESH, IDM_STATS, IDM_DISCONNECT, IDM_GRAB, IDM_RENDER_GDI, IDM_RENDER_GPU,
    IDM_AUDIO, IDM_AUDIO_QUALITY, IDM_AUDIO_SPEED
};

HWND g_view;

/* D3D11 */
static ID3D11Device             *g_dev;
static ID3D11DeviceContext      *g_ctx;
static IDXGISwapChain1          *g_sc;
static ID3D11RenderTargetView   *g_rtv;
static ID3D11Texture2D          *g_tex;
static ID3D11Texture2D          *g_stage[2];   /* CPU から書く送り出し用(STAGE_SIZE 四方、交互に使う) */
static int                       g_stageNext;
#define STAGE_SIZE 1024
static ID3D11DeviceContext4     *g_ctx4;        /* 送り出し用を GPU が読み終えたかをフェンスで知る */
static ID3D11Fence              *g_fence;
static HANDLE                    g_fenceEv;
static UINT64                    g_fenceVal, g_stageFence[2];
static ID3D11ShaderResourceView *g_srv;
static ID3D11VertexShader       *g_vs;
static ID3D11PixelShader        *g_ps;
static ID3D11PixelShader        *g_psNv12;     /* NV12 → RGB(GPU で復号した絵を g_tex へ写す) */
static ID3D11RenderTargetView   *g_rtvTex;     /* g_tex の 0 番のミップ */
static ID3D11Texture2D          *g_frameTex;   /* 復号した絵の写し(NV12。復号器の大きさ) */
static ID3D11ShaderResourceView *g_srvY, *g_srvUV;
static int                       g_frameW, g_frameH;
static SRWLOCK                   g_frameLock = SRWLOCK_INIT;
static volatile LONG             g_frameNew;   /* g_frameTex に新しい絵が入った */
static ID3D11SamplerState       *g_sampLinear, *g_sampPoint;
static int                       g_texW, g_texH;
static BOOL                      g_d3d, g_d3dFailed, g_tearing;
static HWND                      g_canvas;      /* スワップチェーンを結びつける子窓 */
#define CANVAS_CLASS L"iiv.Client.Canvas"

/* 表示 */
static RECT    g_dst;           /* 絵を置く矩形(クライアント座標) */
static double  g_scale = 1.0;
static int     g_scrollX, g_scrollY;
static BOOL    g_full;
static WINDOWPLACEMENT g_wpPrev;
static LONG    g_stylePrev;
static BOOL    g_connected;

/* 入力 */
static int      g_btnMask;
static int      g_wheel, g_hwheel;
static BYTE     g_keys[256];
static UINT     g_downScan[256];        /* 押したときのスキャン コード(離すときも同じものを送る) */
static BOOL     g_downExt[256], g_downAny[256];
static HHOOK    g_hook;
static HCURSOR  g_remoteCursor, g_dotCursor;
static int      g_cursorVer = -1;
static double   g_cursorScale;
static DWORD    g_lastLocalMove;
static HWND     g_bar;                  /* 全画面のときの帯 */
static HFONT    g_barFont;
static HBRUSH   g_barBrush;
static int      g_lastRX, g_lastRY;     /* 最後に送ったポインタの位置 */

/* 統計と検証 */
static DWORD    g_statTick;
static LONG     g_statUpdates;
static LONG64   g_statBytes;
static int      g_presented, g_statPresented;
static WCHAR    g_statText[96];
static DWORD    g_lastFrameTick;
static DWORD    g_connectTick;

void app_connection_closed(WCHAR *reason);   /* main.c */
void app_reconnect(void);
extern ConnParams g_params;

/* ------------------------------------------------------------------ */
/*  D3D11                                                               */
/* ------------------------------------------------------------------ */

#define SAFE_RELEASE(p, T) do { if (p) { T##_Release(p); (p) = NULL; } } while (0)

static void d3d_release_target(void)
{
    SAFE_RELEASE(g_rtv, ID3D11RenderTargetView);
}

static void d3d_release(void)
{
    AcquireSRWLockExclusive(&g_frameLock);
    SAFE_RELEASE(g_srvY, ID3D11ShaderResourceView);
    SAFE_RELEASE(g_srvUV, ID3D11ShaderResourceView);
    SAFE_RELEASE(g_frameTex, ID3D11Texture2D);
    g_frameW = g_frameH = 0;
    g_frameNew = 0;
    g_d3d = FALSE;
    ReleaseSRWLockExclusive(&g_frameLock);
    SAFE_RELEASE(g_rtvTex, ID3D11RenderTargetView);
    SAFE_RELEASE(g_psNv12, ID3D11PixelShader);
    d3d_release_target();
    SAFE_RELEASE(g_srv, ID3D11ShaderResourceView);
    SAFE_RELEASE(g_tex, ID3D11Texture2D);
    SAFE_RELEASE(g_stage[0], ID3D11Texture2D);
    SAFE_RELEASE(g_stage[1], ID3D11Texture2D);
    SAFE_RELEASE(g_fence, ID3D11Fence);
    SAFE_RELEASE(g_ctx4, ID3D11DeviceContext4);
    if (g_fenceEv) { CloseHandle(g_fenceEv); g_fenceEv = NULL; }
    g_fenceVal = g_stageFence[0] = g_stageFence[1] = 0;
    SAFE_RELEASE(g_sampLinear, ID3D11SamplerState);
    SAFE_RELEASE(g_sampPoint, ID3D11SamplerState);
    SAFE_RELEASE(g_vs, ID3D11VertexShader);
    SAFE_RELEASE(g_ps, ID3D11PixelShader);
    SAFE_RELEASE(g_sc, IDXGISwapChain1);
    SAFE_RELEASE(g_ctx, ID3D11DeviceContext);
    SAFE_RELEASE(g_dev, ID3D11Device);
    if (g_canvas && IsWindow(g_canvas)) DestroyWindow(g_canvas);
    g_canvas = NULL;
    g_texW = g_texH = 0;
    g_d3d = FALSE;
}

static void d3d_draw(void);

static LRESULT CALLBACK canvas_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        EndPaint(h, &ps);
        d3d_draw();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* 親の窓の中身いっぱいに子窓を出す */
static BOOL canvas_create(HWND parent)
{
    static BOOL registered;
    RECT c;
    if (!registered) {
        WNDCLASSW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.lpfnWndProc = canvas_proc;
        wc.hInstance = g_inst;
        wc.lpszClassName = CANVAS_CLASS;
        registered = RegisterClassW(&wc) != 0;
    }
    GetClientRect(parent, &c);
    g_canvas = CreateWindowExW(0, CANVAS_CLASS, L"", WS_CHILD | WS_VISIBLE | WS_DISABLED,
                               0, 0, max(c.right, 1), max(c.bottom, 1), parent, NULL, g_inst, NULL);
    return g_canvas != NULL;
}

static BOOL d3d_init(HWND hwnd)
{
    D3D_FEATURE_LEVEL     fl, want[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    IDXGIDevice          *dd = NULL;
    IDXGIAdapter         *ad = NULL;
    IDXGIFactory2        *fac = NULL;
    IDXGIFactory5        *f5 = NULL;
    DXGI_SWAP_CHAIN_DESC1 sd;
    D3D11_SAMPLER_DESC    smp;
    HRESULT hr;

    if (g_d3d) return TRUE;
    if (g_d3dFailed || g_cfg.renderGdi) return FALSE;
    /* 動画の機能(GPU での H.264 の復号)付きで作る。駄目なら無しで */
    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                           want, ARRAYSIZE(want), D3D11_SDK_VERSION, &g_dev, &fl, &g_ctx);
    if (FAILED(hr))
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, want, ARRAYSIZE(want),
                               D3D11_SDK_VERSION, &g_dev, &fl, &g_ctx);
    if (FAILED(hr)) goto fail;
    {
        /* 復号器(通信のスレッド)も同じデバイスを使う */
        ID3D10Multithread *mt = NULL;
        if (SUCCEEDED(ID3D11Device_QueryInterface(g_dev, &IID_ID3D10Multithread, (void **)&mt))) {
            ID3D10Multithread_SetMultithreadProtected(mt, TRUE);
            ID3D10Multithread_Release(mt);
        }
    }
    hr = E_FAIL;
    if (!canvas_create(hwnd)) goto fail;
    if (FAILED(ID3D11Device_QueryInterface(g_dev, &IID_IDXGIDevice, (void **)&dd))) goto fail;
    {
        IDXGIDevice1 *d1 = NULL;
        if (SUCCEEDED(IDXGIDevice_QueryInterface(dd, &IID_IDXGIDevice1, (void **)&d1))) {
            IDXGIDevice1_SetMaximumFrameLatency(d1, 1);     /* GPU に溜める描画は 1 つまで(遅れとメモリを減らす) */
            IDXGIDevice1_Release(d1);
        }
    }
    IDXGIDevice_GetAdapter(dd, &ad);
    IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory2, (void **)&fac);
    if (!fac) goto fail;
    if (SUCCEEDED(IDXGIFactory2_QueryInterface(fac, &IID_IDXGIFactory5, (void **)&f5))) {
        BOOL allow = FALSE;
        if (SUCCEEDED(IDXGIFactory5_CheckFeatureSupport(f5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow)))) g_tearing = allow;
        IDXGIFactory5_Release(f5);
    }
    ZeroMemory(&sd, sizeof(sd));
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sd.Flags = g_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    hr = IDXGIFactory2_CreateSwapChainForHwnd(fac, (IUnknown *)g_dev, g_canvas, &sd, NULL, NULL, &g_sc);
    if (FAILED(hr)) goto fail;
    IDXGIFactory2_MakeWindowAssociation(fac, g_canvas, DXGI_MWA_NO_ALT_ENTER);
    if (FAILED(ID3D11Device_CreateVertexShader(g_dev, g_vsCode, sizeof(g_vsCode), NULL, &g_vs)) ||
        FAILED(ID3D11Device_CreatePixelShader(g_dev, g_psCode, sizeof(g_psCode), NULL, &g_ps)) ||
        FAILED(ID3D11Device_CreatePixelShader(g_dev, g_psNv12Code, sizeof(g_psNv12Code), NULL, &g_psNv12))) goto fail;
    ZeroMemory(&smp, sizeof(smp));
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11Device_CreateSamplerState(g_dev, &smp, &g_sampLinear);
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    smp.MaxLOD = 0;
    ID3D11Device_CreateSamplerState(g_dev, &smp, &g_sampPoint);
    /* 送り出し用を GPU がまだ読んでいる間に Map すると、ドライバが待つ間ずっと CPU を回す
       (1 回 3〜9M サイクル。2026-10-06 実測、RTX 3070 Ti)。フェンスで読み終わりを知り、
       まだならイベントで眠って待つ。D3D11.4(Windows 10 1703 以降)が無ければ今までどおり。 */
    {
        ID3D11Device5 *dev5 = NULL;
        if (SUCCEEDED(ID3D11Device_QueryInterface(g_dev, &IID_ID3D11Device5, (void **)&dev5))) {
            if (FAILED(ID3D11DeviceContext_QueryInterface(g_ctx, &IID_ID3D11DeviceContext4, (void **)&g_ctx4)) ||
                FAILED(ID3D11Device5_CreateFence(dev5, 0, D3D11_FENCE_FLAG_NONE, &IID_ID3D11Fence, (void **)&g_fence)) ||
                !(g_fenceEv = CreateEventW(NULL, FALSE, FALSE, NULL))) {
                SAFE_RELEASE(g_fence, ID3D11Fence);
                SAFE_RELEASE(g_ctx4, ID3D11DeviceContext4);
            }
            ID3D11Device5_Release(dev5);
        }
    }
    IDXGIFactory2_Release(fac);
    IDXGIAdapter_Release(ad);
    IDXGIDevice_Release(dd);
    g_d3d = TRUE;
    log_printf(L"D3D11 で描く(機能レベル %x、ティアリング %d)", (unsigned)fl, g_tearing);
    return TRUE;
fail:
    log_printf(L"D3D11 が使えないので GDI で描く (0x%08lX)", (unsigned long)hr);
    if (fac) IDXGIFactory2_Release(fac);
    if (ad) IDXGIAdapter_Release(ad);
    if (dd) IDXGIDevice_Release(dd);
    d3d_release();
    g_d3dFailed = TRUE;
    return FALSE;
}

static BOOL d3d_texture(int w, int h)
{
    D3D11_TEXTURE2D_DESC td;
    if (g_tex && g_texW == w && g_texH == h) return TRUE;
    SAFE_RELEASE(g_rtvTex, ID3D11RenderTargetView);
    SAFE_RELEASE(g_srv, ID3D11ShaderResourceView);
    SAFE_RELEASE(g_tex, ID3D11Texture2D);
    SAFE_RELEASE(g_stage[0], ID3D11Texture2D);
    SAFE_RELEASE(g_stage[1], ID3D11Texture2D);
    ZeroMemory(&td, sizeof(td));
    td.Width = (UINT)w;
    td.Height = (UINT)h;
    td.MipLevels = 0;               /* ミップマップを全部 */
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_tex)) ||
        FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)g_tex, NULL, &g_srv))) {
        SAFE_RELEASE(g_tex, ID3D11Texture2D);
        return FALSE;
    }
    {
        D3D11_RENDER_TARGET_VIEW_DESC rd;
        ZeroMemory(&rd, sizeof(rd));
        rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        rd.Texture2D.MipSlice = 0;
        ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)g_tex, &rd, &g_rtvTex);
    }
    td.Width = (UINT)min(w, STAGE_SIZE);
    td.Height = (UINT)min(h, STAGE_SIZE);
    td.MipLevels = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.MiscFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_stage[0]))) g_stage[0] = NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_stage[1]))) g_stage[1] = NULL;
    g_texW = w;
    g_texH = h;
    return TRUE;
}

static BOOL d3d_target(void)
{
    ID3D11Texture2D *bb = NULL;
    if (g_rtv) return TRUE;
    if (FAILED(IDXGISwapChain1_GetBuffer(g_sc, 0, &IID_ID3D11Texture2D, (void **)&bb))) return FALSE;
    ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)bb, NULL, &g_rtv);
    ID3D11Texture2D_Release(bb);
    return g_rtv != NULL;
}

static void d3d_draw(void)
{
    static const float black[4] = { 0, 0, 0, 1 };
    D3D11_VIEWPORT vp;
    HRESULT hr;
    if (!g_d3d || !g_tex || !d3d_target()) return;
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &g_rtv, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(g_ctx, g_rtv, black);
    vp.TopLeftX = (float)g_dst.left;
    vp.TopLeftY = (float)g_dst.top;
    vp.Width = (float)(g_dst.right - g_dst.left);
    vp.Height = (float)(g_dst.bottom - g_dst.top);
    vp.MinDepth = 0;
    vp.MaxDepth = 1;
    ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetInputLayout(g_ctx, NULL);
    ID3D11DeviceContext_VSSetShader(g_ctx, g_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_ctx, g_ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &g_srv);
    ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 1, g_scale == 1.0 ? &g_sampPoint : &g_sampLinear);
    ID3D11DeviceContext_Draw(g_ctx, 3, 0);
    hr = IDXGISwapChain1_Present(g_sc, 0, g_tearing && g_full ? DXGI_PRESENT_ALLOW_TEARING : 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        log_printf(L"GPU を失った。作り直す");
        d3d_release();
        if (d3d_init(g_view)) {
            AcquireSRWLockExclusive(&g_rm.lock);
            SetRect(&g_rm.dirty, 0, 0, g_rm.w, g_rm.h);
            ReleaseSRWLockExclusive(&g_rm.lock);
        }
    }
    g_presented++;
}

/* 通信のスレッド(vdec.c)から: GPU で復号した絵(tex の sub 番)を写しておく。描くのは窓のスレッド */
BOOL view_submit_nv12(void *tex, UINT sub, int w, int h)
{
    BOOL ok = FALSE;
    AcquireSRWLockExclusive(&g_frameLock);
    if (g_d3d && g_dev) {
        if (!g_frameTex || g_frameW != w || g_frameH != h) {
            D3D11_TEXTURE2D_DESC td;
            D3D11_SHADER_RESOURCE_VIEW_DESC sd;
            SAFE_RELEASE(g_srvY, ID3D11ShaderResourceView);
            SAFE_RELEASE(g_srvUV, ID3D11ShaderResourceView);
            SAFE_RELEASE(g_frameTex, ID3D11Texture2D);
            ZeroMemory(&td, sizeof(td));
            td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = 1; td.ArraySize = 1;
            td.Format = DXGI_FORMAT_NV12; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            ZeroMemory(&sd, sizeof(sd));
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = 1;
            if (SUCCEEDED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_frameTex))) {
                sd.Format = DXGI_FORMAT_R8_UNORM;
                ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)g_frameTex, &sd, &g_srvY);
                sd.Format = DXGI_FORMAT_R8G8_UNORM;
                ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)g_frameTex, &sd, &g_srvUV);
            }
            if (!g_srvY || !g_srvUV) {
                SAFE_RELEASE(g_srvY, ID3D11ShaderResourceView);
                SAFE_RELEASE(g_srvUV, ID3D11ShaderResourceView);
                SAFE_RELEASE(g_frameTex, ID3D11Texture2D);
                log_printf(L"NV12 の絵を作れない(%dx%d)", w, h);
            } else {
                g_frameW = w;
                g_frameH = h;
            }
        }
        if (g_frameTex) {
            ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)g_frameTex, 0, 0, 0, 0, (ID3D11Resource *)tex, sub, NULL);
            InterlockedExchange(&g_frameNew, 1);
            ok = TRUE;
        }
    }
    ReleaseSRWLockExclusive(&g_frameLock);
    return ok;
}

/* 復号した絵(NV12)を RGB にして g_tex へ(シェーダーで)。写したら TRUE */
static BOOL d3d_convert(void)
{
    D3D11_VIEWPORT vp;
    ID3D11ShaderResourceView *srvs[2], *none[2] = { NULL, NULL };
    BOOL done = FALSE;
    if (!g_frameNew) return FALSE;
    AcquireSRWLockExclusive(&g_frameLock);
    if (g_frameTex && g_rtvTex && g_srvY && g_srvUV) {
        vp.TopLeftX = 0; vp.TopLeftY = 0;
        vp.Width = (float)g_texW; vp.Height = (float)g_texH;
        vp.MinDepth = 0; vp.MaxDepth = 1;
        srvs[0] = g_srvY;
        srvs[1] = g_srvUV;
        ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &g_rtvTex, NULL);
        ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);
        ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_IASetInputLayout(g_ctx, NULL);
        ID3D11DeviceContext_VSSetShader(g_ctx, g_vs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(g_ctx, g_psNv12, NULL, 0);
        ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 2, srvs);
        ID3D11DeviceContext_Draw(g_ctx, 3, 0);
        ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 2, none);
        ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 0, NULL, NULL);
        InterlockedExchange(&g_frameNew, 0);
        done = TRUE;
    }
    ReleaseSRWLockExclusive(&g_frameLock);
    if (done && g_scale < 1.0 && g_srv) ID3D11DeviceContext_GenerateMips(g_ctx, g_srv);
    return done;
}

/* 変わった範囲をテクスチャへ */
static void d3d_upload(RECT r)
{
    RECT all;
    int  tx, ty, w, h;
    if (!g_d3d) return;
    AcquireSRWLockShared(&g_rm.lock);
    w = g_rm.w;
    h = g_rm.h;
    ReleaseSRWLockShared(&g_rm.lock);
    if (!d3d_texture(w, h)) return;
    SetRect(&all, 0, 0, w, h);
    IntersectRect(&r, &r, &all);
    /* 送り出し領域の大きさのタイルに分け、2 枚を交互に使って送る。
       GPU を待つ Map の間は画面の写しの鍵を持たない(復号のスレッドを止めない) */
    for (ty = r.top; ty < r.bottom; ty += STAGE_SIZE)
        for (tx = r.left; tx < r.right; tx += STAGE_SIZE) {
            int tw = min(STAGE_SIZE, (int)r.right - tx), th = min(STAGE_SIZE, (int)r.bottom - ty), y;
            int si = g_stageNext;
            ID3D11Texture2D *st = g_stage[si];
            D3D11_MAPPED_SUBRESOURCE m;
            D3D11_BOX box;
            g_stageNext ^= 1;
            if (g_fence && st && ID3D11Fence_GetCompletedValue(g_fence) < g_stageFence[si] &&
                SUCCEEDED(ID3D11Fence_SetEventOnCompletion(g_fence, g_stageFence[si], g_fenceEv))) {
                ID3D11DeviceContext_Flush(g_ctx);
                WaitForSingleObject(g_fenceEv, 500);
            }
            if (!st || FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)st, 0, D3D11_MAP_WRITE, 0, &m))) continue;
            AcquireSRWLockShared(&g_rm.lock);
            if (g_rm.w == w && g_rm.h == h)
                for (y = 0; y < th; y++)
                    memcpy((BYTE *)m.pData + (size_t)y * m.RowPitch, g_rm.fb + ((size_t)(ty + y) * w + tx) * 4, (size_t)tw * 4);
            ReleaseSRWLockShared(&g_rm.lock);
            ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)st, 0);
            box.left = 0; box.top = 0; box.front = 0; box.right = (UINT)tw; box.bottom = (UINT)th; box.back = 1;
            ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)g_tex, 0, (UINT)tx, (UINT)ty, 0,
                                                      (ID3D11Resource *)st, 0, &box);
            if (g_fence && SUCCEEDED(ID3D11DeviceContext4_Signal(g_ctx4, g_fence, g_fenceVal + 1)))
                g_stageFence[si] = ++g_fenceVal;
        }
    if (g_scale < 1.0 && g_srv) ID3D11DeviceContext_GenerateMips(g_ctx, g_srv);
}

/* ------------------------------------------------------------------ */
/*  配置                                                                */
/* ------------------------------------------------------------------ */

static void update_scrollbars(int cw, int ch)
{
    SCROLLINFO si;
    BOOL needH = !g_cfg.fit && !g_full && g_rm.w > cw, needV = !g_cfg.fit && !g_full && g_rm.h > ch;
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = needH ? g_rm.w - 1 : 0;
    si.nPage = needH ? (UINT)cw : 0;
    si.nPos = g_scrollX;
    SetScrollInfo(g_view, SB_HORZ, &si, TRUE);
    si.nMax = needV ? g_rm.h - 1 : 0;
    si.nPage = needV ? (UINT)ch : 0;
    si.nPos = g_scrollY;
    SetScrollInfo(g_view, SB_VERT, &si, TRUE);
    ShowScrollBar(g_view, SB_HORZ, needH);
    ShowScrollBar(g_view, SB_VERT, needV);
}

static void layout(void)
{
    RECT c;
    int  cw, ch, w = g_rm.w, h = g_rm.h;
    GetClientRect(g_view, &c);
    cw = c.right;
    ch = c.bottom;
    if (!w || !h || !cw || !ch) { SetRectEmpty(&g_dst); return; }
    if (g_cfg.fit || g_full) {
        double sx = (double)cw / w, sy = (double)ch / h;
        int dw, dh;
        g_scale = sx < sy ? sx : sy;
        if (g_scale > 0.999 && g_scale < 1.001) g_scale = 1.0;
        dw = (int)(w * g_scale + 0.5);
        dh = (int)(h * g_scale + 0.5);
        SetRect(&g_dst, (cw - dw) / 2, (ch - dh) / 2, (cw - dw) / 2 + dw, (ch - dh) / 2 + dh);
        g_scrollX = g_scrollY = 0;
    } else {
        g_scale = 1.0;
        if (g_scrollX > w - cw) g_scrollX = max(0, w - cw);
        if (g_scrollY > h - ch) g_scrollY = max(0, h - ch);
        SetRect(&g_dst, w < cw ? (cw - w) / 2 : -g_scrollX, h < ch ? (ch - h) / 2 : -g_scrollY, 0, 0);
        g_dst.right = g_dst.left + w;
        g_dst.bottom = g_dst.top + h;
    }
    if (!g_full) update_scrollbars(cw, ch);
    {
        /* 大きさの記録(変わったときだけ。窓に合わせた絵が小さい、という報告の手がかり) */
        static int lcw, lch, lw, lh, lfit;
        if (cw != lcw || ch != lch || w != lw || h != lh || g_cfg.fit != lfit) {
            lcw = cw; lch = ch; lw = w; lh = h; lfit = g_cfg.fit;
            log_printf(L"配置: 窓の中身 %dx%d、相手 %dx%d、%s、倍率 %.3f、絵 (%ld,%ld)-(%ld,%ld)、%s%s",
                       cw, ch, w, h, g_cfg.fit || g_full ? L"窓に合わせる" : L"等倍", g_scale,
                       g_dst.left, g_dst.top, g_dst.right, g_dst.bottom, g_d3d ? L"GPU" : L"GDI",
                       IsZoomed(g_view) ? L"、最大化" : g_full ? L"、全画面" : L"");
        }
    }
}

/* 初めて出すときの大きさ: 等倍で収まれば等倍、収まらなければ作業領域の 9 割に縮める */
static void initial_size(void)
{
    HMONITOR    hm = MonitorFromWindow(g_view, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    RECT        r;
    UINT        dpi = GetDpiForWindow(g_view);
    int         aw, ah, w = g_rm.w, h = g_rm.h;
    DWORD       st = (DWORD)GetWindowLongW(g_view, GWL_STYLE), ex = (DWORD)GetWindowLongW(g_view, GWL_EXSTYLE);
    double      s = 1.0;

    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(hm, &mi);
    SetRect(&r, 0, 0, 0, 0);
    AdjustWindowRectExForDpi(&r, st & ~(WS_HSCROLL | WS_VSCROLL), FALSE, ex, dpi);
    aw = (int)((mi.rcWork.right - mi.rcWork.left) * 0.92) - (r.right - r.left);
    ah = (int)((mi.rcWork.bottom - mi.rcWork.top) * 0.92) - (r.bottom - r.top);
    if (w > aw || h > ah) {
        double sx = (double)aw / w, sy = (double)ah / h;
        s = sx < sy ? sx : sy;
        if (!g_cfg.fit) s = 1.0;
    }
    w = (int)(w * s);
    h = (int)(h * s);
    if (!g_cfg.fit) { w = min(w, aw); h = min(h, ah); }
    r.left = 0; r.top = 0; r.right = w; r.bottom = h;
    AdjustWindowRectExForDpi(&r, st & ~(WS_HSCROLL | WS_VSCROLL), FALSE, ex, dpi);
    SetWindowPos(g_view, NULL,
                 mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - (r.right - r.left)) / 2,
                 mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - (r.bottom - r.top)) / 2,
                 r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
}

void view_set_title(void)
{
    WCHAR t[512];
    if (!g_connected) _snwprintf(t, ARRAYSIZE(t), L"%s に接続しています - %s", g_params.host, APP_TITLE);
    else if (g_cfg.showStats && g_statText[0]) _snwprintf(t, ARRAYSIZE(t), L"%s - %s  [%s]", g_rm.name, APP_TITLE, g_statText);
    else _snwprintf(t, ARRAYSIZE(t), L"%s - %s%s", g_rm.name, APP_TITLE, g_params.viewOnly ? L"(見るだけ)" : L"");
    t[ARRAYSIZE(t) - 1] = 0;
    SetWindowTextW(g_view, t);
}

/* ------------------------------------------------------------------ */
/*  全画面とキーボードの横取り                                          */
/* ------------------------------------------------------------------ */

static BOOL want_grab(void)
{
    if (g_params.viewOnly || !g_connected) return FALSE;
    if (g_cfg.grab == GRAB_ALWAYS) return TRUE;
    if (g_cfg.grab == GRAB_FULLSCREEN) return g_full;
    return FALSE;
}

static void key_event(UINT vk, UINT scan, BOOL ext, BOOL down);

static LRESULT CALLBACK ll_keyboard(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && GetForegroundWindow() == g_view && want_grab()) {
        const KBDLLHOOKSTRUCT *k = (const KBDLLHOOKSTRUCT *)lp;
        if (!(k->flags & LLKHF_INJECTED) || g_hookTest) {
            BOOL down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
            if (g_hookTest) log_printf(L"[hook] vk=%02X scan=%02X flags=%02X %s", k->vkCode, k->scanCode, k->flags, down ? L"down" : L"up");
            key_event(k->vkCode, k->scanCode, (k->flags & LLKHF_EXTENDED) != 0, down);
            return 1;
        }
    }
    return CallNextHookEx(g_hook, code, wp, lp);
}

static void update_hook(void)
{
    BOOL want = want_grab() && GetForegroundWindow() == g_view;
    if (want && !g_hook) g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, ll_keyboard, g_inst, 0);
    else if (!want && g_hook) { UnhookWindowsHookEx(g_hook); g_hook = NULL; }
}

/* ------------------------------------------------------------------ */
/*  全画面のときの帯                                                    */
/* ------------------------------------------------------------------ */

static void show_menu(int x, int y);

static LRESULT CALLBACK bar_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDB_RESTORE: ShowWindow(h, SW_HIDE); if (g_full) view_toggle_fullscreen(); break;
        case IDB_MENU: {
            RECT r;
            GetWindowRect(h, &r);
            show_menu(r.left, r.bottom);
            break;
        }
        case IDB_MIN:   ShowWindow(h, SW_HIDE); ShowWindow(g_view, SW_MINIMIZE); break;
        case IDB_CLOSE: PostMessageW(g_view, WM_CLOSE, 0, 0); break;
        }
        return 0;
    case WM_ERASEBKGND: {
        RECT r;
        GetClientRect(h, &r);
        FillRect((HDC)wp, &r, g_barBrush);
        return 1;
    }
    case WM_CTLCOLORSTATIC:             /* 速さの文字: 帯の色の上に明るい灰色で */
        SetTextColor((HDC)wp, RGB(220, 220, 220));
        SetBkColor((HDC)wp, RGB(43, 43, 43));
        return (LRESULT)g_barBrush;
    case WM_CTLCOLORBTN:
        return (LRESULT)g_barBrush;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void bar_show(BOOL show)
{
    static const struct { int id; const WCHAR *text; int w; } k_btn[] = {
        { IDB_RESTORE, L"窓に戻す (Ctrl+Alt+Enter)", 190 }, { IDB_MENU, L"メニュー (F8)", 120 },
        { IDB_MIN, L"最小化", 80 }, { IDB_CLOSE, L"切断", 80 },
    };
    MONITORINFO mi;
    UINT dpi;
    int  i, x, total = 0, pad, bh;

    if (!show) {
        if (g_bar) ShowWindow(g_bar, SW_HIDE);
        KillTimer(g_view, TIMER_BARHIDE);
        return;
    }
    dpi = GetDpiForWindow(g_view);
    pad = MulDiv(6, (int)dpi, 96);
    bh = MulDiv(30, (int)dpi, 96);
    if (!g_bar) {
        WNDCLASSW wc;
        LOGFONTW  lf;
        ZeroMemory(&wc, sizeof(wc));
        wc.lpfnWndProc = bar_proc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.lpszClassName = BAR_CLASS;
        RegisterClassW(&wc);
        g_barBrush = CreateSolidBrush(RGB(43, 43, 43));
        g_bar = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, BAR_CLASS, L"", WS_POPUP,
                                0, 0, 10, 10, g_view, NULL, g_inst, NULL);
        SystemParametersInfoForDpi(SPI_GETICONTITLELOGFONT, sizeof(lf), &lf, 0, dpi);
        g_barFont = CreateFontIndirectW(&lf);
        for (i = 0; i < (int)ARRAYSIZE(k_btn); i++) {
            HWND b = CreateWindowExW(0, L"BUTTON", k_btn[i].text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                     0, 0, 10, 10, g_bar, (HMENU)(INT_PTR)k_btn[i].id, g_inst, NULL);
            SendMessageW(b, WM_SETFONT, (WPARAM)g_barFont, FALSE);
            SetWindowTheme(b, L"DarkMode_Explorer", NULL);
        }
        {
            /* 速さ(「速さを表示」のときだけ出す。stats_tick が書き換える) */
            HWND st = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX,
                                      0, 0, 10, 10, g_bar, (HMENU)(INT_PTR)IDB_STATS, g_inst, NULL);
            SendMessageW(st, WM_SETFONT, (WPARAM)g_barFont, FALSE);
        }
    }
    for (i = 0; i < (int)ARRAYSIZE(k_btn); i++) total += MulDiv(k_btn[i].w, (int)dpi, 96) + pad;
    if (g_cfg.showStats) total += MulDiv(170, (int)dpi, 96) + pad;
    total += pad;
    x = pad;
    for (i = 0; i < (int)ARRAYSIZE(k_btn); i++) {
        int w = MulDiv(k_btn[i].w, (int)dpi, 96);
        SetWindowPos(GetDlgItem(g_bar, k_btn[i].id), NULL, x, pad / 2, w, bh - pad, SWP_NOZORDER | SWP_NOACTIVATE);
        x += w + pad;
    }
    {
        HWND st = GetDlgItem(g_bar, IDB_STATS);
        SetWindowTextW(st, g_statText);
        SetWindowPos(st, NULL, x + pad, pad / 2, MulDiv(170, (int)dpi, 96) - pad, bh - pad, SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(st, g_cfg.showStats ? SW_SHOWNOACTIVATE : SW_HIDE);
    }
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(g_view, MONITOR_DEFAULTTONEAREST), &mi);
    SetWindowPos(g_bar, HWND_TOPMOST, mi.rcMonitor.left + (mi.rcMonitor.right - mi.rcMonitor.left - total) / 2,
                 mi.rcMonitor.top, total, bh, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetTimer(g_view, TIMER_BARHIDE, 300, NULL);
    log_printf(L"全画面の帯を出した");
}

/* 帯から離れたら消す */
static void bar_check_hide(void)
{
    RECT  r;
    POINT p;
    if (!g_bar || !IsWindowVisible(g_bar)) { KillTimer(g_view, TIMER_BARHIDE); return; }
    GetWindowRect(g_bar, &r);
    InflateRect(&r, 0, (r.bottom - r.top) / 2);
    GetCursorPos(&p);
    if (!PtInRect(&r, p) && !g_hookTest) bar_show(FALSE);
}

void view_toggle_fullscreen(void)
{
    if (!g_full) {
        MONITORINFO mi;
        g_wpPrev.length = sizeof(g_wpPrev);
        GetWindowPlacement(g_view, &g_wpPrev);
        g_stylePrev = GetWindowLongW(g_view, GWL_STYLE);
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromWindow(g_view, MONITOR_DEFAULTTONEAREST), &mi);
        g_full = TRUE;
        ShowScrollBar(g_view, SB_BOTH, FALSE);
        SetWindowLongW(g_view, GWL_STYLE, (g_stylePrev & ~(WS_OVERLAPPEDWINDOW | WS_HSCROLL | WS_VSCROLL)) | WS_POPUP);
        SetWindowPos(g_view, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        g_full = FALSE;
        bar_show(FALSE);
        SetWindowLongW(g_view, GWL_STYLE, g_stylePrev);
        SetWindowPlacement(g_view, &g_wpPrev);
        SetWindowPos(g_view, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    layout();
    update_hook();
    InvalidateRect(g_view, NULL, FALSE);
}

/* ------------------------------------------------------------------ */
/*  キー                                                                */
/* ------------------------------------------------------------------ */

static void send_key(UINT vk, BOOL down)
{
    conn_send_key(down, vk, g_downScan[vk], g_downExt[vk]);
}

void view_release_keys(void)
{
    int i;
    for (i = 0; i < 256; i++) {
        if (!g_downAny[i]) continue;
        send_key((UINT)i, FALSE);
        g_downAny[i] = FALSE;
    }
    ZeroMemory(g_keys, sizeof(g_keys));
    if (g_btnMask) {
        g_btnMask = 0;
        conn_send_pointer(0, g_lastRX, g_lastRY);
    }
}

/* 「音を鳴らす」の項目の文字(サーバー側で鳴らせないときは、そう書く) */
static const WCHAR *audio_menu_text(void)
{
    if (g_cfg.audio && g_connected) {
        switch (audio_server_status()) {
        case IIV_AS_DISABLED: return L"音を鳴らす(&U)  ― サーバーの「音を鳴らす」が切られています";
        case IIV_AS_NODEVICE: return L"音を鳴らす(&U)  ― サーバーで音を取り込めません";
        case -1:              return L"音を鳴らす(&U)  ― サーバーから返事がありません(古い版か、まだ)";
        }
    }
    return L"音を鳴らす(&U)";
}

static void update_checks(HMENU m)
{
    ModifyMenuW(m, IDM_AUDIO, MF_BYCOMMAND | MF_STRING | (g_cfg.audio ? MF_CHECKED : MF_UNCHECKED), IDM_AUDIO, audio_menu_text());
    CheckMenuRadioItem(m, IDM_AUDIO_QUALITY, IDM_AUDIO_SPEED, g_cfg.audioSpeed ? IDM_AUDIO_SPEED : IDM_AUDIO_QUALITY, MF_BYCOMMAND);
    CheckMenuItem(m, IDM_FULLSCREEN, MF_BYCOMMAND | (g_full ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_FIT, MF_BYCOMMAND | (g_cfg.fit ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_ACTUAL, MF_BYCOMMAND | (!g_cfg.fit ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_Q_HIGH, MF_BYCOMMAND | (g_cfg.quality == Q_AUTO ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_Q_LOSSLESS, MF_BYCOMMAND | (g_cfg.quality == Q_LAN ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_Q_NORMAL, MF_BYCOMMAND | (g_cfg.quality == Q_WIFI ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_Q_LOW, MF_BYCOMMAND | (g_cfg.quality == Q_SLOW ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_GRAB, MF_BYCOMMAND | (g_cfg.grab == GRAB_ALWAYS ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_VIEWONLY, MF_BYCOMMAND | (g_params.viewOnly ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_STATS, MF_BYCOMMAND | (g_cfg.showStats ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuRadioItem(m, IDM_RENDER_GDI, IDM_RENDER_GPU, g_cfg.renderGdi ? IDM_RENDER_GDI : IDM_RENDER_GPU, MF_BYCOMMAND);
    EnableMenuItem(m, IDM_CAD, MF_BYCOMMAND | (g_params.viewOnly ? MF_GRAYED : MF_ENABLED));
    EnableMenuItem(m, IDM_SENDF8, MF_BYCOMMAND | (g_params.viewOnly ? MF_GRAYED : MF_ENABLED));
}

static void show_menu(int x, int y);

static void key_event(UINT vk, UINT scan, BOOL ext, BOOL down)
{
    UINT side = vk;
    if (vk >= 256) return;
    /* 自分で持つキーボードの状態(ToUnicodeEx に渡す) */
    if (vk == VK_SHIFT) side = scan == 0x36 ? VK_RSHIFT : VK_LSHIFT;
    else if (vk == VK_CONTROL) side = ext ? VK_RCONTROL : VK_LCONTROL;
    else if (vk == VK_MENU) side = ext ? VK_RMENU : VK_LMENU;
    g_keys[side] = down ? 0x80 : 0;
    if (side == VK_LSHIFT || side == VK_RSHIFT) g_keys[VK_SHIFT] = (g_keys[VK_LSHIFT] | g_keys[VK_RSHIFT]);
    if (side == VK_LCONTROL || side == VK_RCONTROL) g_keys[VK_CONTROL] = (g_keys[VK_LCONTROL] | g_keys[VK_RCONTROL]);
    if (side == VK_LMENU || side == VK_RMENU) g_keys[VK_MENU] = (g_keys[VK_LMENU] | g_keys[VK_RMENU]);
    if (vk == VK_CAPITAL && down) g_keys[VK_CAPITAL] ^= 1;
    if (vk == VK_CAPITAL || vk == VK_NUMLOCK) g_keys[vk] = (BYTE)((GetKeyState((int)vk) & 1) | (down ? 0x80 : 0));

    /* こちらで使うキー */
    if (down && vk == VK_RETURN)
        log_printf(L"Enter: Ctrl %d(左 %d 右 %d)、Alt %d(左 %d 右 %d)、全画面 %d", g_keys[VK_CONTROL] >> 7,
                   g_keys[VK_LCONTROL] >> 7, g_keys[VK_RCONTROL] >> 7, g_keys[VK_MENU] >> 7, g_keys[VK_LMENU] >> 7,
                   g_keys[VK_RMENU] >> 7, g_full);
    if (down && vk == VK_RETURN && (g_keys[VK_CONTROL] & 0x80) && (g_keys[VK_MENU] & 0x80)) {
        view_release_keys();
        view_toggle_fullscreen();
        return;
    }
    if (down && vk == VK_F8 && !(g_keys[VK_CONTROL] & 0x80) && !(g_keys[VK_MENU] & 0x80) && !(g_keys[VK_SHIFT] & 0x80)) {
        RECT r;
        GetWindowRect(g_view, &r);
        show_menu(r.left + 40, r.top + 40);
        return;
    }
    if (g_params.viewOnly || !g_connected) return;

    if (down) {
        if (!scan) scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        g_downScan[vk] = scan;
        g_downExt[vk] = ext;
        g_downAny[vk] = TRUE;
        send_key(vk, TRUE);
        /* 半角/全角 は離した知らせが来ないことがあるので、すぐ離す */
        if (vk == 0xF3 || vk == 0xF4) { send_key(vk, FALSE); g_downAny[vk] = FALSE; }
    } else if (g_downAny[vk]) {
        send_key(vk, FALSE);
        g_downAny[vk] = FALSE;
    }
}

static void send_cad(void)
{
    if (g_params.viewOnly) return;
    conn_send_sas();
}

/* ------------------------------------------------------------------ */
/*  マウス                                                              */
/* ------------------------------------------------------------------ */

static BOOL to_remote(int cx, int cy, int *rx, int *ry)
{
    int w = g_dst.right - g_dst.left, h = g_dst.bottom - g_dst.top;
    if (w <= 0 || h <= 0) return FALSE;
    *rx = (int)((cx - g_dst.left) * (double)g_rm.w / w);
    *ry = (int)((cy - g_dst.top) * (double)g_rm.h / h);
    if (*rx < 0) *rx = 0;
    if (*ry < 0) *ry = 0;
    if (*rx >= g_rm.w) *rx = g_rm.w - 1;
    if (*ry >= g_rm.h) *ry = g_rm.h - 1;
    return TRUE;
}

static void pointer(LPARAM lp)
{
    int x, y;
    if (g_params.viewOnly || !g_connected) return;
    if (to_remote((short)LOWORD(lp), (short)HIWORD(lp), &x, &y)) {
        g_lastRX = x;
        g_lastRY = y;
        conn_send_pointer(g_btnMask, x, y);
    }
}

static void wheel(LPARAM lp, int delta, BOOL horizontal)
{
    POINT p;
    int  *acc = horizontal ? &g_hwheel : &g_wheel, x, y;
    if (g_params.viewOnly || !g_connected) return;
    p.x = (short)LOWORD(lp);
    p.y = (short)HIWORD(lp);
    ScreenToClient(g_view, &p);
    if (!to_remote(p.x, p.y, &x, &y)) return;
    (void)acc;
    conn_send_wheel(g_btnMask, x, y, delta, horizontal);   /* 量をそのまま送る(高精度のホイールも細かく伝わる) */
}

/* ------------------------------------------------------------------ */
/*  カーソル                                                            */
/* ------------------------------------------------------------------ */

static HCURSOR make_cursor(const BYTE *bgra, int w, int h, int hx, int hy, double scale)
{
    int        sw = max(1, (int)(w * scale + 0.5)), sh = max(1, (int)(h * scale + 0.5)), x, y;
    BITMAPV5HEADER bi;
    BYTE      *bits = NULL;
    HDC        dc = GetDC(NULL);
    HBITMAP    col, msk;
    ICONINFO   ii;
    HCURSOR    hc;

    ZeroMemory(&bi, sizeof(bi));
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = sw;
    bi.bV5Height = -sh;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    col = CreateDIBSection(dc, (BITMAPINFO *)&bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    ReleaseDC(NULL, dc);
    if (!col) return NULL;
    for (y = 0; y < sh; y++)
        for (x = 0; x < sw; x++) {
            const BYTE *s = bgra + ((size_t)(y * h / sh) * w + (x * w / sw)) * 4;
            BYTE *d = bits + ((size_t)y * sw + x) * 4;
            d[3] = s[3];
            d[0] = s[3] ? s[0] : 0; d[1] = s[3] ? s[1] : 0; d[2] = s[3] ? s[2] : 0;
        }
    msk = CreateBitmap(sw, sh, 1, 1, NULL);
    ii.fIcon = FALSE;
    ii.xHotspot = (DWORD)min(sw - 1, (int)(hx * scale));
    ii.yHotspot = (DWORD)min(sh - 1, (int)(hy * scale));
    ii.hbmMask = msk;
    ii.hbmColor = col;
    hc = (HCURSOR)CreateIconIndirect(&ii);
    DeleteObject(col);
    DeleteObject(msk);
    return hc;
}

static void refresh_cursor(BOOL force)
{
    BYTE *copy = NULL;
    int   w, h, hx, hy, ver;
    double sc = g_scale;
    AcquireSRWLockShared(&g_rm.lock);
    if (g_rm.deskW > 0 && g_rm.w > 0) sc *= (double)g_rm.w / g_rm.deskW;    /* 形は相手の画面の画素の大きさで来る */
    ver = g_rm.curVer;
    w = g_rm.curW; h = g_rm.curH; hx = g_rm.curHotX; hy = g_rm.curHotY;
    if (!force && ver == g_cursorVer && sc == g_cursorScale) { ReleaseSRWLockShared(&g_rm.lock); return; }
    if (g_rm.curPix && w && h) {
        copy = (BYTE *)malloc((size_t)w * h * 4);
        if (copy) memcpy(copy, g_rm.curPix, (size_t)w * h * 4);
    }
    ReleaseSRWLockShared(&g_rm.lock);
    g_cursorVer = ver;
    g_cursorScale = sc;
    if (g_remoteCursor) DestroyCursor(g_remoteCursor);
    g_remoteCursor = NULL;
    if (copy) {
        /* 見える画素が無ければ NULL のまま(カーソルを消す) */
        int i, vis = 0;
        for (i = 0; i < w * h; i++) if (copy[i * 4 + 3]) { vis = 1; break; }
        if (vis) g_remoteCursor = make_cursor(copy, w, h, hx, hy, sc < 0.3 ? 0.3 : sc);
        free(copy);
    }
}

static HCURSOR dot_cursor(void)
{
    BYTE px[5 * 5 * 4];
    int i;
    for (i = 0; i < 25; i++) {
        int x = i % 5, y = i / 5, edge = x == 0 || y == 0 || x == 4 || y == 4;
        BOOL corner = (x == 0 || x == 4) && (y == 0 || y == 4);
        px[i * 4 + 0] = px[i * 4 + 1] = px[i * 4 + 2] = (BYTE)(edge ? 0 : 255);
        px[i * 4 + 3] = (BYTE)(corner ? 0 : 255);
    }
    return make_cursor(px, 5, 5, 2, 2, 1.0);
}

/* ------------------------------------------------------------------ */
/*  検証用: 絵を BMP に書く                                             */
/* ------------------------------------------------------------------ */

/* GPU で描いているときは、表示用のテクスチャ(g_tex の 0 番のミップ)を fb へ読み戻す */
static void d3d_readback(void)
{
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D *st = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    int y;
    if (!g_d3d || !g_tex) return;
    ZeroMemory(&td, sizeof(td));
    td.Width = (UINT)g_texW; td.Height = (UINT)g_texH; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &st))) return;
    ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)st, 0, 0, 0, 0, (ID3D11Resource *)g_tex, 0, NULL);
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
        AcquireSRWLockExclusive(&g_rm.lock);
        if (g_rm.fb && g_rm.w == g_texW && g_rm.h == g_texH)
            for (y = 0; y < g_texH; y++) memcpy(g_rm.fb + (size_t)y * g_texW * 4, (BYTE *)m.pData + (size_t)y * m.RowPitch, (size_t)g_texW * 4);
        ReleaseSRWLockExclusive(&g_rm.lock);
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)st, 0);
    }
    ID3D11Texture2D_Release(st);
}

static void dump_bmp(const WCHAR *path)
{
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    HANDLE f;
    DWORD wr;
    int y;
    if (vdec_is_gpu()) d3d_readback();
    AcquireSRWLockShared(&g_rm.lock);
    ZeroMemory(&fh, sizeof(fh));
    ZeroMemory(&ih, sizeof(ih));
    ih.biSize = sizeof(ih);
    ih.biWidth = g_rm.w;
    ih.biHeight = -g_rm.h;
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + (DWORD)g_rm.w * g_rm.h * 4;
    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        WriteFile(f, &fh, sizeof(fh), &wr, NULL);
        WriteFile(f, &ih, sizeof(ih), &wr, NULL);
        for (y = 0; y < g_rm.h; y++) WriteFile(f, g_rm.fb + (size_t)y * g_rm.w * 4, (DWORD)g_rm.w * 4, &wr, NULL);
        CloseHandle(f);
    }
    ReleaseSRWLockShared(&g_rm.lock);
    log_printf(L"絵を書いた: %s", path);
}

static void finish_test(void)
{
    static BOOL done;
    LARGE_INTEGER f;
    double sec = (GetTickCount() - g_connectTick) / 1000.0;
    if (done) return;
    done = TRUE;
    QueryPerformanceFrequency(&f);
    if (g_dumpPath[0]) dump_bmp(g_dumpPath);
    log_printf(L"検証の終わり: %.2f 秒で 更新 %ld 回(%.1f 回/秒)、受信 %I64d バイト(%.1f Mbps)、"
               L"受信と復号 平均 %.2fms、描画 %d 回",
               sec, g_rm.updates, g_rm.updates / (sec > 0 ? sec : 1), g_rm.bytes, g_rm.bytes * 8 / (sec > 0 ? sec : 1) / 1e6,
               g_rm.updates ? g_rm.decodeTicks * 1000.0 / (double)f.QuadPart / g_rm.updates : 0.0, g_presented);
    if (audio_playing()) {
        WCHAR s[256];
        audio_stats(s, ARRAYSIZE(s));
        log_printf(L"検証の終わり: %s", s);
    }
    PostMessageW(g_view, WM_CLOSE, 0, 0);
}

/* ------------------------------------------------------------------ */
/*  メニュー                                                            */
/* ------------------------------------------------------------------ */

static void build_menu(HMENU m)
{
    HMENU q = CreatePopupMenu(), r = CreatePopupMenu(), a = CreatePopupMenu();
    AppendMenuW(q, MF_STRING | (g_cfg.quality == Q_AUTO ? MF_CHECKED : 0), IDM_Q_HIGH, L"自動(おすすめ)");
    AppendMenuW(q, MF_STRING | (g_cfg.quality == Q_LAN ? MF_CHECKED : 0), IDM_Q_LOSSLESS, L"高め(LAN 向け)");
    AppendMenuW(q, MF_STRING | (g_cfg.quality == Q_WIFI ? MF_CHECKED : 0), IDM_Q_NORMAL, L"控えめ(Wi-Fi・遠隔地)");
    AppendMenuW(q, MF_STRING | (g_cfg.quality == Q_SLOW ? MF_CHECKED : 0), IDM_Q_LOW, L"細い回線");
    AppendMenuW(m, MF_STRING | (g_full ? MF_CHECKED : 0), IDM_FULLSCREEN, L"全画面(&F)\tCtrl+Alt+Enter");
    AppendMenuW(m, MF_STRING | (g_cfg.fit ? MF_CHECKED : 0), IDM_FIT, L"窓に合わせる(&W)");
    AppendMenuW(m, MF_STRING | (!g_cfg.fit ? MF_CHECKED : 0), IDM_ACTUAL, L"等倍(&A)");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)q, L"画質(&Q)");
    AppendMenuW(r, MF_STRING, IDM_RENDER_GDI, L"GDI(メモリが少ない)");
    AppendMenuW(r, MF_STRING, IDM_RENDER_GPU, L"GPU(縮めても文字がきれい)");
    CheckMenuRadioItem(r, IDM_RENDER_GDI, IDM_RENDER_GPU, g_cfg.renderGdi ? IDM_RENDER_GDI : IDM_RENDER_GPU, MF_BYCOMMAND);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)r, L"描画(&G)");
    AppendMenuW(m, MF_STRING | (g_cfg.audio ? MF_CHECKED : 0), IDM_AUDIO, audio_menu_text());
    AppendMenuW(a, MF_STRING, IDM_AUDIO_QUALITY, L"音質優先(そのまま送る。1.5Mbps)");
    AppendMenuW(a, MF_STRING, IDM_AUDIO_SPEED, L"速度優先(音質を落として 0.1Mbps。音が少し遅れる)");
    CheckMenuRadioItem(a, IDM_AUDIO_QUALITY, IDM_AUDIO_SPEED, g_cfg.audioSpeed ? IDM_AUDIO_SPEED : IDM_AUDIO_QUALITY, MF_BYCOMMAND);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)a, L"音質(&O)");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (g_params.viewOnly ? MF_GRAYED : 0), IDM_CAD, L"Ctrl+Alt+Del を送る(&C)");
    AppendMenuW(m, MF_STRING | (g_params.viewOnly ? MF_GRAYED : 0), IDM_SENDF8, L"F8 を送る(&8)");
    AppendMenuW(m, MF_STRING | (g_cfg.grab == GRAB_ALWAYS ? MF_CHECKED : 0), IDM_GRAB, L"窓でも Windows キーなどを送る(&K)");
    AppendMenuW(m, MF_STRING | (g_params.viewOnly ? MF_CHECKED : 0), IDM_VIEWONLY, L"見るだけ(&V)");
    AppendMenuW(m, MF_STRING, IDM_REFRESH, L"画面を取り直す(&R)");
    AppendMenuW(m, MF_STRING | (g_cfg.showStats ? MF_CHECKED : 0), IDM_STATS, L"速さを表示(&S)");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_DISCONNECT, L"切断(&D)");
}

static void show_menu(int x, int y)
{
    HMENU m = CreatePopupMenu();
    build_menu(m);
    view_release_keys();
    TrackPopupMenu(m, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON, x, y, 0, g_view, NULL);
    DestroyMenu(m);
}

/* 描画の方式を切り替える(つないだまま) */
static void set_render(BOOL gdi)
{
    if (g_cfg.renderGdi == gdi) return;
    g_cfg.renderGdi = gdi;
    config_save();
    if (gdi) {
        d3d_release();
    } else {
        g_d3dFailed = FALSE;
        if (g_connected && d3d_init(g_view)) {
            RECT all;
            AcquireSRWLockShared(&g_rm.lock);
            SetRect(&all, 0, 0, g_rm.w, g_rm.h);
            ReleaseSRWLockShared(&g_rm.lock);
            d3d_upload(all);
        }
    }
    log_printf(L"描画を %s に切り替えた", g_d3d ? L"GPU(D3D11)" : L"GDI");
    conn_reset_decoder();
    InvalidateRect(g_view, NULL, FALSE);
}

static void command(int id)
{
    switch (id) {
    case IDM_FULLSCREEN: view_toggle_fullscreen(); break;
    case IDM_FIT:    g_cfg.fit = TRUE; layout(); InvalidateRect(g_view, NULL, FALSE); config_save(); break;
    case IDM_ACTUAL: g_cfg.fit = FALSE; layout(); InvalidateRect(g_view, NULL, FALSE); config_save(); break;
    case IDM_Q_HIGH: case IDM_Q_LOSSLESS: case IDM_Q_NORMAL: case IDM_Q_LOW:
        g_cfg.quality = id - IDM_Q_HIGH;
        g_params.quality = g_cfg.quality;
        conn_set_quality(g_cfg.quality);
        config_save();
        break;
    case IDM_CAD: send_cad(); break;
    case IDM_SENDF8:
        if (!g_params.viewOnly) { conn_send_key(TRUE, VK_F8, 0x42, FALSE); conn_send_key(FALSE, VK_F8, 0x42, FALSE); }
        break;
    case IDM_GRAB:
        g_cfg.grab = g_cfg.grab == GRAB_ALWAYS ? GRAB_FULLSCREEN : GRAB_ALWAYS;
        update_hook();
        config_save();
        break;
    case IDM_VIEWONLY:
        view_release_keys();
        g_params.viewOnly = !g_params.viewOnly;
        update_hook();
        view_set_title();
        break;
    case IDM_REFRESH: conn_request_keyframe(); break;
    case IDM_STATS:
        g_cfg.showStats = !g_cfg.showStats;
        config_save();
        view_set_title();
        if (g_bar && IsWindowVisible(g_bar)) bar_show(TRUE);    /* 出ている帯も並べ直す */
        break;
    case IDM_DISCONNECT: PostMessageW(g_view, WM_CLOSE, 0, 0); break;
    case IDM_RENDER_GDI: case IDM_RENDER_GPU: set_render(id == IDM_RENDER_GDI); break;
    case IDM_AUDIO:
        g_cfg.audio = !g_cfg.audio;
        config_save();
        conn_set_audio(audio_codec_from_cfg());
        break;
    case IDM_AUDIO_QUALITY: case IDM_AUDIO_SPEED:
        g_cfg.audioSpeed = id == IDM_AUDIO_SPEED;
        config_save();
        if (g_cfg.audio) conn_set_audio(audio_codec_from_cfg());
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  窓                                                                  */
/* ------------------------------------------------------------------ */

static void paint_gdi(HDC dc)
{
    RECT c;
    GetClientRect(g_view, &c);
    if (!g_connected || !g_rm.fb) {
        WCHAR s[300];
        FillRect(dc, &c, (HBRUSH)GetStockObject(BLACK_BRUSH));
        SetTextColor(dc, RGB(220, 220, 220));
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
        _snwprintf(s, ARRAYSIZE(s), L"%s に接続しています…", g_params.host);
        s[ARRAYSIZE(s) - 1] = 0;
        DrawTextW(dc, s, -1, &c, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    {
        BITMAPINFO bi;
        HRGN rgn = CreateRectRgnIndirect(&c), inner = CreateRectRgnIndirect(&g_dst);
        CombineRgn(rgn, rgn, inner, RGN_DIFF);
        FillRgn(dc, rgn, (HBRUSH)GetStockObject(BLACK_BRUSH));
        DeleteObject(rgn);
        DeleteObject(inner);
        ZeroMemory(&bi, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        AcquireSRWLockShared(&g_rm.lock);
        bi.bmiHeader.biWidth = g_rm.w;
        bi.bmiHeader.biHeight = -g_rm.h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, g_scale < 1.0 ? HALFTONE : COLORONCOLOR);
        SetBrushOrgEx(dc, 0, 0, NULL);
        StretchDIBits(dc, g_dst.left, g_dst.top, g_dst.right - g_dst.left, g_dst.bottom - g_dst.top,
                      0, 0, g_rm.w, g_rm.h, g_rm.fb, &bi, DIB_RGB_COLORS, SRCCOPY);
        ReleaseSRWLockShared(&g_rm.lock);
    }
}

static void on_frame(void)
{
    RECT   r;
    UINT32 no;
    LONG64 recv;
    AcquireSRWLockExclusive(&g_rm.lock);
    r = g_rm.dirty;
    SetRectEmpty(&g_rm.dirty);
    g_rm.framePosted = FALSE;
    no = g_rm.frameNo;
    recv = g_rm.frameRecvQpc;
    ReleaseSRWLockExclusive(&g_rm.lock);
    g_lastFrameTick = GetTickCount();
    if (!IsRectEmpty(&r)) {
        if (g_d3d) {
            if (!d3d_convert()) d3d_upload(r);
            d3d_draw();
        } else {
            RECT inv;
            int w = g_dst.right - g_dst.left, h = g_dst.bottom - g_dst.top;
            if (g_rm.w && g_rm.h) {
                inv.left = g_dst.left + (int)((double)r.left * w / g_rm.w) - 1;
                inv.top = g_dst.top + (int)((double)r.top * h / g_rm.h) - 1;
                inv.right = g_dst.left + (int)((double)r.right * w / g_rm.w) + 2;
                inv.bottom = g_dst.top + (int)((double)r.bottom * h / g_rm.h) + 2;
                InvalidateRect(g_view, &inv, FALSE);
                UpdateWindow(g_view);
                g_presented++;
            }
        }
    }
    if (no) conn_frame_shown(no, recv);       /* 表示した(サーバーは返事を見て次を送る) */
    if (g_exitAfter && g_rm.updates >= g_exitAfter) finish_test();
}

/* ------------------------------------------------------------------ */
/*  スリープさせない(nosleep=1)                                         */
/* ------------------------------------------------------------------ */

/* Windows の電源の要求(PowerSetRequest)で、スリープと画面の消灯を止める。
   理由の文字列を付けるので、powercfg /requests で誰が止めているか見える */
static void keep_awake(BOOL on, const WCHAR *reason)
{
    static HANDLE req;
    static BOOL   active;
    if (on == active) return;
    if (!req) {
        REASON_CONTEXT rc;
        ZeroMemory(&rc, sizeof(rc));
        rc.Version = POWER_REQUEST_CONTEXT_VERSION;
        rc.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
        rc.Reason.SimpleReasonString = (LPWSTR)reason;
        req = PowerCreateRequest(&rc);
        if (req == INVALID_HANDLE_VALUE) req = NULL;
        if (!req) { log_printf(L"スリープを止められない (%lu)", GetLastError()); return; }
    }
    if (on) {
        PowerSetRequest(req, PowerRequestSystemRequired);
        PowerSetRequest(req, PowerRequestDisplayRequired);
    } else {
        PowerClearRequest(req, PowerRequestSystemRequired);
        PowerClearRequest(req, PowerRequestDisplayRequired);
    }
    active = on;
    log_printf(on ? L"スリープと画面の消灯を止めた" : L"スリープと画面の消灯の止めを解いた");
}

static void on_connected(void)
{
    g_connected = TRUE;
    keep_awake(g_cfg.noSleep, L"iiv-client: サーバーにつないでいる");
    if (!g_d3d) d3d_init(g_view);
    if (g_d3d) {
        AcquireSRWLockShared(&g_rm.lock);
        d3d_texture(g_rm.w, g_rm.h);
        ReleaseSRWLockShared(&g_rm.lock);
    }
    if (!g_full && !IsZoomed(g_view)) initial_size();
    if (g_cfg.fullscreen && !g_full) view_toggle_fullscreen();
    layout();
    view_set_title();
    ImmAssociateContextEx(g_view, NULL, 0);
    update_hook();
    g_statTick = GetTickCount();
    g_connectTick = g_statTick;
    g_statUpdates = 0;
    g_statBytes = 0;
    SetTimer(g_view, TIMER_STATS, 1000, NULL);
    if (g_idleExitMs) SetTimer(g_view, TIMER_IDLE, 100, NULL);
    g_lastFrameTick = GetTickCount();
    InvalidateRect(g_view, NULL, FALSE);
}

static void stats_tick(void)
{
    DWORD  now = GetTickCount(), el = now - g_statTick;
    LONG   up = g_rm.updates;
    LONG64 by = g_rm.bytes;
    if (el < 500) return;
    _snwprintf(g_statText, ARRAYSIZE(g_statText), L"%.1f fps  %.1f Mbps", (g_presented - g_statPresented) * 1000.0 / el,
               (double)(by - g_statBytes) * 8.0 / el / 1000.0);
    g_statText[ARRAYSIZE(g_statText) - 1] = 0;
    g_statTick = now;
    g_statUpdates = up;
    g_statBytes = by;
    g_statPresented = g_presented;
    if (g_cfg.showStats) {
        view_set_title();
        if (g_bar && IsWindowVisible(g_bar)) SetDlgItemTextW(g_bar, IDB_STATS, g_statText);   /* 全画面の帯にも */
    }
}

static LRESULT CALLBACK view_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        HMENU sys = GetSystemMenu(hwnd, FALSE);
        AppendMenuW(sys, MF_SEPARATOR, 0, NULL);
        build_menu(sys);
        DragAcceptFiles(hwnd, FALSE);
        return 0;
    }

    case WM_APP_CONNECTED:
        on_connected();
        return 0;

    case WM_APP_D3D:                    /* 通信のスレッドから: 復号に使うデバイス(参照を足して渡す。無ければ 0) */
        if (!g_cfg.renderGdi && !g_d3d) d3d_init(hwnd);
        if (g_d3d && g_dev) {
            ID3D11Device_AddRef(g_dev);
            return (LRESULT)g_dev;
        }
        return 0;

    case WM_APP_FRAME:
        on_frame();
        return 0;

    case WM_APP_RESIZE:
        if (g_d3d) {
            AcquireSRWLockShared(&g_rm.lock);
            d3d_texture(g_rm.w, g_rm.h);
            ReleaseSRWLockShared(&g_rm.lock);
        }
        if (!g_full && !IsZoomed(hwnd)) initial_size();
        layout();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_APP_CURSOR:
        refresh_cursor(FALSE);
        {
            POINT p;
            GetCursorPos(&p);
            if (WindowFromPoint(p) == hwnd) SetCursor(g_rm.haveCursorEnc ? g_remoteCursor : g_dotCursor);
        }
        return 0;

    case WM_APP_POINTER:
        /* 相手でカーソルが動いた。こちらで最近動かしていなければ合わせる */
        if (GetForegroundWindow() == hwnd && GetTickCount() - g_lastLocalMove > 300 && g_rm.w) {
            POINT p;
            RECT  c;
            p.x = g_dst.left + (int)((g_rm.ptrX + 0.5) * (g_dst.right - g_dst.left) / g_rm.w);
            p.y = g_dst.top + (int)((g_rm.ptrY + 0.5) * (g_dst.bottom - g_dst.top) / g_rm.h);
            GetClientRect(hwnd, &c);
            if (PtInRect(&c, p)) {
                ClientToScreen(hwnd, &p);
                SetCursorPos(p.x, p.y);
            }
        }
        return 0;

    case WM_APP_BELL:
        MessageBeep(MB_OK);
        return 0;

    case WM_APP_SETCLIP:
        clip_set_from_remote(hwnd, (WCHAR *)lp);
        return 0;

    case WM_APP_FXOFFER:                /* 検証用: 今クリップボードにあるファイルを渡す */
        if (IsClipboardFormatAvailable(CF_HDROP) && OpenClipboard(hwnd)) {
            HDROP hd = (HDROP)GetClipboardData(CF_HDROP);
            if (hd) conn_send_files(hd);
            CloseClipboard();
        } else log_printf(L"[fxoffer] クリップボードにファイルが無い");
        return 0;

    case WM_APP_CLOSED:
        g_connected = FALSE;
        keep_awake(FALSE, NULL);
        KillTimer(hwnd, TIMER_STATS);
        view_release_keys();
        update_hook();
        if (g_dumpPath[0] || g_exitAfter || g_idleExitMs) {
            free((void *)lp);
            finish_test();
            return 0;
        }
        app_connection_closed((WCHAR *)lp);
        return 0;

    case WM_CLIPBOARDUPDATE:
        clip_on_update(hwnd);
        return 0;

    case WM_TIMER:
        if (wp == TIMER_BAR) {
            POINT p;
            RECT  r;
            KillTimer(hwnd, TIMER_BAR);
            GetCursorPos(&p);
            GetWindowRect(hwnd, &r);
            if (g_full && (p.y <= r.top + 1 || g_hookTest)) bar_show(TRUE);    /* 検証では実際のカーソルを見ない */
        } else if (wp == TIMER_BARHIDE) bar_check_hide();
        else if (wp == TIMER_STATS) stats_tick();
        else if (wp == TIMER_IDLE && g_rm.updates > 0 && GetTickCount() - g_lastFrameTick >= (DWORD)g_idleExitMs) {
            KillTimer(hwnd, TIMER_IDLE);
            finish_test();
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (g_d3d && g_connected && g_tex) d3d_draw();
        else paint_gdi(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_SIZE:
        if (g_d3d && g_sc) {
            d3d_release_target();
            MoveWindow(g_canvas, 0, 0, max(LOWORD(lp), 1), max(HIWORD(lp), 1), FALSE);
            IDXGISwapChain1_ResizeBuffers(g_sc, 0, 0, 0, DXGI_FORMAT_UNKNOWN, g_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
        }
        layout();
        refresh_cursor(FALSE);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_HSCROLL:
    case WM_VSCROLL: {
        SCROLLINFO si;
        int bar = msg == WM_HSCROLL ? SB_HORZ : SB_VERT, *pos = msg == WM_HSCROLL ? &g_scrollX : &g_scrollY;
        si.cbSize = sizeof(si);
        si.fMask = SIF_ALL;
        GetScrollInfo(hwnd, bar, &si);
        switch (LOWORD(wp)) {
        case SB_LINEUP:        *pos -= 40; break;
        case SB_LINEDOWN:      *pos += 40; break;
        case SB_PAGEUP:        *pos -= (int)si.nPage; break;
        case SB_PAGEDOWN:      *pos += (int)si.nPage; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: *pos = si.nTrackPos; break;
        }
        if (*pos < 0) *pos = 0;
        layout();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            if (!g_dotCursor) g_dotCursor = dot_cursor();
            if (g_connected && !g_params.viewOnly) {
                refresh_cursor(FALSE);
                SetCursor(g_rm.haveCursorEnc ? g_remoteCursor : g_dotCursor);
            } else {
                SetCursor(LoadCursorW(NULL, IDC_ARROW));
            }
            return TRUE;
        }
        break;

    case WM_MOUSEMOVE:
        g_lastLocalMove = GetTickCount();
        pointer(lp);
        /* 全画面で上端に当てたら、少し待って帯を出す */
        if (g_full && (short)HIWORD(lp) <= 1 && !(g_bar && IsWindowVisible(g_bar))) SetTimer(hwnd, TIMER_BAR, 250, NULL);
        return 0;

    case WM_LBUTTONDOWN: case WM_MBUTTONDOWN: case WM_RBUTTONDOWN: case WM_XBUTTONDOWN:
    case WM_LBUTTONUP:   case WM_MBUTTONUP:   case WM_RBUTTONUP:   case WM_XBUTTONUP:
    case WM_LBUTTONDBLCLK: case WM_MBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_XBUTTONDBLCLK: {
        int bit = 0;
        BOOL down = msg == WM_LBUTTONDOWN || msg == WM_MBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_XBUTTONDOWN ||
                    msg == WM_LBUTTONDBLCLK || msg == WM_MBUTTONDBLCLK || msg == WM_RBUTTONDBLCLK || msg == WM_XBUTTONDBLCLK;
        switch (msg) {
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK: bit = IIV_MB_LEFT; break;
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK: bit = IIV_MB_MIDDLE; break;
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK: bit = IIV_MB_RIGHT; break;
        default: bit = GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? IIV_MB_X1 : IIV_MB_X2; break;
        }
        if (down) g_btnMask |= bit; else g_btnMask &= ~bit;
        if (g_btnMask) SetCapture(hwnd); else ReleaseCapture();
        pointer(lp);
        return msg >= WM_XBUTTONDOWN ? TRUE : 0;
    }

    case WM_MOUSEWHEEL:
        wheel(lp, GET_WHEEL_DELTA_WPARAM(wp), FALSE);
        return 0;
    case WM_MOUSEHWHEEL:
        wheel(lp, GET_WHEEL_DELTA_WPARAM(wp), TRUE);
        return 0;

    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP: {
        BOOL down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
        key_event((UINT)wp, (UINT)((lp >> 16) & 0xFF), (lp & (1 << 24)) != 0, down);
        return 0;
    }
    case WM_SYSCHAR: case WM_CHAR: case WM_DEADCHAR: case WM_SYSDEADCHAR:
        return 0;

    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) < 0xF000 && wp >= IDM_FULLSCREEN && wp <= IDM_AUDIO_SPEED) { command((int)wp); return 0; }
        if ((wp & 0xFFF0) == SC_KEYMENU) return 0;      /* Alt で窓のメニューへ行かない */
        break;

    case WM_INITMENUPOPUP:
        /* 窓のシステム メニューを開くとき、チェックを今の状態に合わせる。
           WM_INITMENU の wParam は GetSystemMenu とは別のハンドル(外側の入れ物)なので使えない */
        if (HIWORD(lp)) update_checks((HMENU)wp);
        break;

    case WM_COMMAND:
        command(LOWORD(wp));
        return 0;

    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) view_release_keys();
        update_hook();
        break;

    case WM_KILLFOCUS:
        view_release_keys();
        break;

    case WM_DPICHANGED: {
        const RECT *r = (const RECT *)lp;
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_CLOSE:
        conn_stop();
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (g_hook) UnhookWindowsHookEx(g_hook);
        g_hook = NULL;
        d3d_release();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND view_create(void)
{
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = view_proc;
    wc.hInstance = g_inst;
    wc.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = NULL;
    wc.lpszClassName = VIEW_CLASS;
    RegisterClassExW(&wc);
    g_view = CreateWindowExW(0, VIEW_CLASS, APP_TITLE, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, 640, 400, NULL, NULL, g_inst, NULL);
    if (g_view) {
        BOOL dark = theme_is_dark();
        if (FAILED(DwmSetWindowAttribute(g_view, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark))))
            DwmSetWindowAttribute(g_view, 19, &dark, sizeof(dark));
        clip_init(g_view);
    }
    return g_view;
}
