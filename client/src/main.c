/* ==================================================================
 * main.c - 起動、引数、接続から表示までの流れ
 *
 *  iiv-client.exe                      接続の画面を出す
 *  iiv-client.exe <サーバー>           すぐにつなぐ(パスワードは覚えたものか、聞く)
 *     サーバーの書き方: 192.168.1.10 / pc-name(ポート 5960)/ host:5961 / [fe80::1]:5961
 *  -password <pw>   パスワード          -viewonly    見るだけ
 *  -fullscreen      全画面で開く        -quality auto|lan|wifi|slow
 *  -ini <path>      別の設定ファイル    -log         ログを書く
 *  -render gdi|gpu  描画の方式(-gdi・-gpu とも書ける)
 *  -remove-firewall 管理者で: ファイアウォールの、この exe の規則を消す(接続の画面のボタンから呼ぶ。
 *                   終了コードは消した数、-1 = 失敗)
 *
 *  検証用:
 *  -dump <file.bmp> 終わるときに受け取った絵を BMP に書く
 *  -exitafter N     N 回の更新を受けたら終わる
 *  -idleexit ms     更新が ms 止まったら終わる
 *  -fxoffer         つながったら、今クリップボードにあるファイルを渡す
 *  -fxnowatch       コピーしたファイルを渡さない(1 台で試すとき、両側が貼り合わないように)
 * ================================================================== */

#include "iivc.h"
#include "resource.h"
#include <shellapi.h>

HINSTANCE  g_inst;
ConnParams g_params;
WCHAR      g_dumpPath[MAX_PATH];
int        g_exitAfter;
int        g_idleExitMs;
BOOL       g_hookTest;
BOOL       g_fxOffer;
BOOL       g_fxNoWatch;

static BOOL g_everConnected;

static BOOL make_params(void)
{
    WCHAR host[256];
    int   port;
    if (!conn_parse_host(g_cfg.host, host, ARRAYSIZE(host), &port)) return FALSE;
    lstrcpynW(g_params.host, host, ARRAYSIZE(g_params.host));
    g_params.port = port;
    lstrcpynA(g_params.password, g_cfg.password, sizeof(g_params.password));
    g_params.quality = g_cfg.quality;
    g_params.viewOnly = g_cfg.viewOnly;
    return TRUE;
}

static void start(void)
{
    view_set_title();
    conn_start(&g_params, g_view);
}

/* 接続の画面を出して、つなぎ直す。やめたら窓を閉じる */
static void ask_and_start(const WCHAR *error)
{
    for (;;) {
        if (!ui_connect_dialog(IsWindowVisible(g_view) ? g_view : NULL, error)) {
            DestroyWindow(g_view);
            return;
        }
        if (make_params()) break;
        error = L"サーバーの書き方が違います。例: 192.168.1.10、pc-name:1、host::5901";
    }
    start();
}

void app_reconnect(void)
{
    start();
}

/* 通信のスレッドが終わった(view.c から) */
void app_connection_closed(WCHAR *reason)
{
    WCHAR msg[600];
    BOOL  needPw = conn_needs_password(), authFail = conn_auth_failed();
    _snwprintf(msg, ARRAYSIZE(msg), L"%s", reason ? reason : L"相手が接続を閉じました。");
    msg[ARRAYSIZE(msg) - 1] = 0;
    free(reason);

    if (needPw || authFail || !g_everConnected) {
        g_cfg.password[0] = 0;
        if (!IsWindowVisible(g_view)) ShowWindow(g_view, SW_HIDE);
        ask_and_start(msg);
        return;
    }
    if (ui_message(g_view, L"接続が切れました。", msg, 1, TD_WARNING_ICON) == IDRETRY) {
        start();
        return;
    }
    DestroyWindow(g_view);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show)
{
    LPWSTR *argv;
    int     argc, i;
    MSG     msg;
    WCHAR   hostArg[256] = L"";
    char    pwArg[PW_MAX] = { 0 };
    BOOL    havePw = FALSE, logArg = FALSE, viewArg = FALSE, fullArg = FALSE, fwRemove = FALSE;
    int     qualityArg = -1, renderArg = -1;  /* renderArg: 1 = GDI、0 = GPU */
    INITCOMMONCONTROLSEX icc;

    (void)prev; (void)cmdline; (void)show;
    g_inst = inst;
    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (i = 1; argv && i < argc; i++) {
        const WCHAR *a = argv[i];
        if (a[0] == L'-' || a[0] == L'/') {
            a++;
            if (a[0] == L'-') a++;
            if (!lstrcmpiW(a, L"ini") && i + 1 < argc) GetFullPathNameW(argv[++i], MAX_PATH, g_iniPath, NULL);
            else if (!lstrcmpiW(a, L"log")) logArg = TRUE;
            else if (!lstrcmpiW(a, L"password") && i + 1 < argc) {
                WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, pwArg, sizeof(pwArg), NULL, NULL);
                pwArg[sizeof(pwArg) - 1] = 0;
                havePw = TRUE;
            }
            else if (!lstrcmpiW(a, L"viewonly")) viewArg = TRUE;
            else if (!lstrcmpiW(a, L"fullscreen")) fullArg = TRUE;
            else if (!lstrcmpiW(a, L"quality") && i + 1 < argc) {
                const WCHAR *q = argv[++i];
                qualityArg = !lstrcmpiW(q, L"lan") ? Q_LAN : !lstrcmpiW(q, L"wifi") ? Q_WIFI : !lstrcmpiW(q, L"slow") ? Q_SLOW : Q_AUTO;
            }
            else if (!lstrcmpiW(a, L"dump") && i + 1 < argc) GetFullPathNameW(argv[++i], MAX_PATH, g_dumpPath, NULL);
            else if (!lstrcmpiW(a, L"exitafter") && i + 1 < argc) g_exitAfter = _wtoi(argv[++i]);
            else if (!lstrcmpiW(a, L"idleexit") && i + 1 < argc) g_idleExitMs = _wtoi(argv[++i]);
            else if (!lstrcmpiW(a, L"hooktest")) g_hookTest = TRUE;
            else if (!lstrcmpiW(a, L"fxoffer")) g_fxOffer = TRUE;
            else if (!lstrcmpiW(a, L"fxnowatch")) g_fxNoWatch = TRUE;
            else if (!lstrcmpiW(a, L"remove-firewall")) fwRemove = TRUE;
            else if (!lstrcmpiW(a, L"gdi")) renderArg = 1;
            else if (!lstrcmpiW(a, L"gpu")) renderArg = 0;
            else if (!lstrcmpiW(a, L"render") && i + 1 < argc) renderArg = lstrcmpiW(argv[++i], L"gpu") != 0;
        } else {
            lstrcpynW(hostArg, a, ARRAYSIZE(hostArg));
        }
    }
    if (argv) LocalFree(argv);

    config_init();
    config_load();
    if (logArg) g_cfg.log = TRUE;
    log_open();
    if (fwRemove) return fw_remove(NULL);
    theme_init();
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);
    log_printf(L"iiv-client %s 起動 (設定 %s)", APP_VERSION, g_iniPath);

    if (qualityArg >= 0) g_cfg.quality = qualityArg;
    if (renderArg >= 0) g_cfg.renderGdi = renderArg;
    if (viewArg) g_cfg.viewOnly = TRUE;
    if (fullArg) g_cfg.fullscreen = TRUE;

    if (!view_create()) return 1;

    if (hostArg[0]) {
        lstrcpynW(g_cfg.host, hostArg, ARRAYSIZE(g_cfg.host));
        if (havePw) lstrcpynA(g_cfg.password, pwArg, sizeof(g_cfg.password));
        else if (!config_saved_password(g_cfg.host, g_cfg.password, sizeof(g_cfg.password))) g_cfg.password[0] = 0;
        if (make_params()) {
            /* 検証で動かすときは、利用者の作業中の窓から手前を奪わない */
            ShowWindow(g_view, (g_dumpPath[0] || g_exitAfter || g_idleExitMs) ? SW_SHOWNOACTIVATE : SW_SHOW);
            start();
        } else {
            ask_and_start(L"サーバーの書き方が違います。");
        }
    } else {
        ask_and_start(NULL);
    }
    if (IsWindow(g_view) && !IsWindowVisible(g_view)) {
        ShowWindow(g_view, SW_SHOW);
    }

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_APP_CONNECTED) g_everConnected = TRUE;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    conn_stop();
    log_printf(L"iiv-client 終了");
    return 0;
}
