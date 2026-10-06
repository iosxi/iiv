/* ==================================================================
 * iivc.h - iiv-client 全体で使う宣言
 *
 *  構成
 *    main.c     起動、引数、接続から表示までの流れ
 *    config.c   iiv-client.ini の読み書き、ログ
 *    conn.c     通信のスレッド(iiv の手順、受信、送信)
 *    vdec.c     H.264 の復号(Media Foundation)と、NV12 → BGRX
 *    view.c     表示の窓(D3D11 / GDI)、拡大縮小、全画面、キー・マウス
 *    clip.c     クリップボードの受け渡し
 *    filexfer.c ファイルのコピー＆貼り付け(iiv-server と同じファイル)
 *    ui.c       接続の画面
 *    theme.c    ライト/ダークの配色(kotemado と同じもの)
 *    fwrules.c  ファイアウォールの、この exe の規則(iiv-server と同じファイル)
 *    zdeflate.c zinflate.c  共通部品(iiv-server と同じもの)
 *  通信の取り決めは iivproto.h(iiv-server と同じファイル)。
 * ================================================================== */
#ifndef IIVC_H
#define IIVC_H

#ifndef UNICODE
#error "UNICODE を定義してビルドする(build.bat は /DUNICODE を付けている)"
#endif

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "zlite.h"
#include "iivproto.h"

#define APP_NAME      L"iiv-client"
#define APP_VERSION   L"1.2.0"

#define WM_APP_CONNECTED  (WM_APP + 1)  /* 初期化まで済んだ */
#define WM_APP_FRAME      (WM_APP + 2)  /* フレームを 1 つ復号した */
#define WM_APP_RESIZE     (WM_APP + 3)  /* 相手の画面の大きさが変わった */
#define WM_APP_CURSOR     (WM_APP + 4)  /* カーソルの形が来た */
#define WM_APP_CLOSED     (WM_APP + 5)  /* 切れた。lParam = 理由(malloc した WCHAR*、無ければ NULL) */
#define WM_APP_SETCLIP    (WM_APP + 6)  /* lParam = 相手から来た文字(malloc した WCHAR*) */
#define WM_APP_POINTER    (WM_APP + 7)  /* 相手がカーソルを動かした */
#define WM_APP_BELL       (WM_APP + 8)
#define WM_APP_FXOFFER    (WM_APP + 9)  /* 検証用(-fxoffer): 今クリップボードにあるファイルを相手へ渡す */
#define WM_APP_D3D        (WM_APP + 10) /* 通信のスレッド → 窓: 復号に使う D3D11 のデバイスをくれ(参照を足して返す) */

/* ------------------------------------------------------------------ */
/*  設定(config.c)                                                     */
/* ------------------------------------------------------------------ */

enum { Q_AUTO, Q_LAN, Q_WIFI, Q_SLOW, Q_COUNT };     /* 求めるビットレート(conn.c の k_kbps) */
enum { GRAB_FULLSCREEN, GRAB_ALWAYS, GRAB_NEVER };

#define MAX_HISTORY 16
#define PW_MAX      200                 /* パスワードの UTF-8 の最大(64 文字) */

typedef struct Config {
    WCHAR host[256];            /* 今回の接続先 */
    char  password[PW_MAX];     /* UTF-8 */
    BOOL  savePassword;
    int   quality;              /* Q_AUTO など */
    BOOL  viewOnly;
    BOOL  fullscreen;
    BOOL  fit;                  /* 窓に合わせて縮める(FALSE = 等倍) */
    int   grab;                 /* システムのキーを相手へ送るとき */
    BOOL  showStats;            /* タイトルに速さを出す */
    BOOL  noSleep;              /* 1 = つないでいる間はスリープさせず、画面も消さない */
    BOOL  renderGdi;            /* 1 = GDI で描く、0 = D3D11(既定) */
    int   theme;
    int   log;
    WCHAR history[MAX_HISTORY][256];
    int   nhistory;
} Config;

extern Config    g_cfg;
extern WCHAR     g_iniPath[MAX_PATH];
extern WCHAR     g_exeDir[MAX_PATH];
extern HINSTANCE g_inst;

/* 検証用(main.c) */
extern WCHAR g_dumpPath[MAX_PATH];  /* -dump: 終わるときに絵を BMP に書く */
extern int   g_exitAfter;           /* -exitafter N: N フレームで終わる(0 = 無し) */
extern int   g_idleExitMs;          /* -idleexit ms: フレームが止まってこの時間で終わる */
extern BOOL  g_hookTest;            /* -hooktest: 注入したキーもフックで横取りする(検証用) */
extern BOOL  g_fxOffer;             /* -fxoffer: つながったら、今クリップボードにあるファイルを渡す(検証用) */
extern BOOL  g_fxNoWatch;           /* -fxnowatch: コピーしたファイルを渡さない(検証用) */

/* fwrules.c: Windows ファイアウォールの、この exe の規則(iiv-server と同じファイル) */
typedef struct { int count, allow, block; long allowProfiles, blockProfiles; } FwInfo;
BOOL fw_query(const WCHAR *keep, FwInfo *fi);
int  fw_remove(const WCHAR *keep);
int  fw_remove_elevated(HWND owner, const WCHAR *keep, const WCHAR *args);
void fw_describe(const FwInfo *fi, WCHAR *s, int cap);

void config_init(void);
void config_load(void);
BOOL config_save(void);
void config_add_history(const WCHAR *host);
BOOL config_saved_password(const WCHAR *host, char *out, int cap);
void config_set_password(const WCHAR *host, const char *pw);   /* pw が空なら消す */
void log_open(void);
void log_printf(const WCHAR *fmt, ...);
char  *utf16_to_utf8(const WCHAR *s, int *outLen);
WCHAR *utf8_to_utf16(const char *s, int len);

/* ------------------------------------------------------------------ */
/*  相手の画面(conn.c / vdec.c)                                        */
/* ------------------------------------------------------------------ */

typedef struct Remote {
    SRWLOCK lock;               /* fb の作り直しと、描画の写しを守る */
    int     w, h;               /* 映像(fb)の大きさ */
    int     deskW, deskH;       /* 相手の取り込んだ範囲(マウスの座標はこの大きさで送る) */
    BYTE   *fb;                 /* BGRX、1 行 = w*4 */
    WCHAR   name[256];
    /* 前回の描画から変わった範囲(lock で守る) */
    RECT    dirty;
    BOOL    framePosted;
    UINT32  frameNo;            /* fb に入っているフレームの番号 */
    LONG64  frameRecvQpc;       /* そのフレームを受け取り始めた時刻 */
    /* カーソル(lock で守る) */
    int     curW, curH, curHotX, curHotY, curVer;
    BYTE   *curPix;             /* BGRA(A = 0 か 255) */
    BOOL    haveCursorEnc;      /* 相手がカーソルを送ってくる */
    int     ptrX, ptrY;         /* 相手が動かしたカーソルの位置(映像の座標) */
    /* 統計 */
    volatile LONG64 bytes;
    volatile LONG   updates;    /* 復号したフレームの数 */
    volatile LONG64 decodeTicks;
} Remote;

extern Remote g_rm;

typedef struct ConnParams {
    WCHAR host[256];
    int   port;
    char  password[PW_MAX];
    int   quality;
    BOOL  viewOnly;
} ConnParams;

BOOL conn_parse_host(const WCHAR *in, WCHAR *host, int hostCap, int *port);
void conn_start(const ConnParams *p, HWND notify);
void conn_stop(void);
BOOL conn_active(void);
void conn_set_quality(int q);
void conn_send_pointer(int buttons, int x, int y);              /* x, y は映像の座標。buttons は IIV_MB_* */
void conn_send_wheel(int buttons, int x, int y, int delta, BOOL horizontal);
void conn_send_key(BOOL down, UINT vk, UINT scan, BOOL ext);
void conn_send_sas(void);
void conn_request_keyframe(void);
void conn_frame_shown(UINT32 frame, LONG64 recvQpc);           /* 表示した(サーバーへ返事を返す) */
void conn_send_clipboard(const char *utf8, int len);            /* こちらのクリップボードが変わった */
void conn_send_files(HDROP hd);                                 /* こちらでファイルがコピーされた */
void conn_reset_decoder(void);                                  /* 描画の方式が変わった: 次のフレームで復号器を作り直す */
const WCHAR *conn_last_error(void);
BOOL conn_auth_failed(void);
BOOL conn_needs_password(void);

/* filexfer.c: ファイルのコピー＆貼り付け(iiv-server と同じファイル) */
#define FX_MAX          (16 << 20)          /* 1 つのメッセージの中身の上限 */
enum { FX_HELLO = 1, FX_FILES, FX_READ, FX_DATA };
BOOL fx_make_offer(const int *conns, int nconn, HDROP hd, BYTE **out, int *outLen);
BOOL fx_make_offer_paths(const int *conns, int nconn, const WCHAR *paths, BYTE **out, int *outLen);
WCHAR *fx_hdrop_paths(HDROP hd);
HANDLE fx_host_user_token(void);
void fx_request(int conn, const BYTE *p, int n);
void fx_deliver(int conn, const BYTE *p, int n);
void fx_conn_closed(int conn);
void fx_offer_received(int conn, const BYTE *p, int n);
BOOL fx_clipboard_is_ours(void);
void fx_stop(void);
BOOL fx_host_send(int conn, int sub, const BYTE *p, int n);

/* vdec.c: d3dDevice があれば GPU(DXVA)で復号して view_submit_nv12 へ、無ければ CPU で fb へ */
BOOL vdec_open(int w, int h, void *d3dDevice);
BOOL vdec_is_gpu(void);
BOOL vdec_decode(const BYTE *data, int len, BOOL *gotFrame);    /* 復号できたら fb に書いて *gotFrame = TRUE */
void vdec_close(void);
const WCHAR *vdec_name(void);

/* ------------------------------------------------------------------ */
/*  表示(view.c)                                                       */
/* ------------------------------------------------------------------ */

extern HWND g_view;

HWND view_create(void);
void view_set_title(void);
void view_toggle_fullscreen(void);
void view_release_keys(void);
BOOL view_submit_nv12(void *tex, UINT sub, int w, int h);       /* 通信のスレッドから: GPU で復号した絵 */

/* clip.c */
void clip_init(HWND hwnd);
void clip_on_update(HWND hwnd);
void clip_set_from_remote(HWND hwnd, WCHAR *text);
void clip_get_current(char **utf8, int *len);

/* ui.c */
BOOL ui_connect_dialog(HWND owner, const WCHAR *error);   /* FALSE = やめた */
int  ui_message(HWND owner, const WCHAR *main, const WCHAR *content, int buttons, PCWSTR icon);

/* theme.c */
void     theme_init(void);
BOOL     theme_refresh(void);
BOOL     theme_is_dark(void);
COLORREF theme_back(void);
COLORREF theme_footer(void);
COLORREF theme_ctrl_back(void);
COLORREF theme_text(void);
COLORREF theme_dim_text(void);
COLORREF theme_line(void);
HBRUSH   theme_back_brush(void);
HBRUSH   theme_footer_brush(void);
HBRUSH   theme_ctrl_brush(void);
void     theme_allow_dark(HWND hwnd);
void     theme_apply_dialog(HWND dlg);
LRESULT  theme_ctlcolor(UINT msg, HDC dc, HWND ctl, BOOL dimText);
BOOL     theme_custom_draw_button(NMCUSTOMDRAW *cd, LRESULT *result);

#endif
