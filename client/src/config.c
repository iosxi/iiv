/* ==================================================================
 * config.c - iiv-client.ini の読み書きとログ
 *
 *  レジストリは使わない。設定は exe と同じ場所の iiv-client.ini
 *  (UTF-8 のテキスト)だけ。-ini で別のファイルを指定できる。
 *
 *  覚えたパスワードは Windows の DPAPI(CryptProtectData)で暗号にして 16 進で置く。
 *  戻せるのは、同じ PC に同じ利用者でログインしているときだけ。
 * ================================================================== */

#include "iivc.h"
#include <wincrypt.h>

#pragma comment(lib, "crypt32.lib")
#include <stdarg.h>

Config g_cfg;
WCHAR  g_iniPath[MAX_PATH];
WCHAR  g_exeDir[MAX_PATH];

#define MAX_PW 32

typedef struct { WCHAR host[256]; char *hex; } SavedPw;      /* hex は malloc */

static SavedPw          g_pw[MAX_PW];
static int              g_npw;
static CRITICAL_SECTION g_logCs;
static HANDLE           g_logFile = INVALID_HANDLE_VALUE;

static const char *const k_quality[Q_COUNT] = { "auto", "lan", "wifi", "slow" };
static const char *const k_grab[3] = { "fullscreen", "always", "never" };

void config_init(void)
{
    WCHAR *p;
    GetModuleFileNameW(NULL, g_exeDir, MAX_PATH);
    p = wcsrchr(g_exeDir, L'\\');
    if (p) *p = 0;
    if (!g_iniPath[0]) wsprintfW(g_iniPath, L"%s\\iiv-client.ini", g_exeDir);
    InitializeCriticalSection(&g_logCs);
    ZeroMemory(&g_cfg, sizeof(g_cfg));
    g_cfg.quality = Q_AUTO;
    g_cfg.fit = TRUE;
    g_cfg.grab = GRAB_FULLSCREEN;
    g_cfg.renderGdi = FALSE;
    g_cfg.noSleep = TRUE;
}

char *utf16_to_utf8(const WCHAR *s, int *outLen)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    char *r = (char *)malloc(n > 0 ? (size_t)n : 1);
    if (!r) return NULL;
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s, -1, r, n, NULL, NULL);
    else r[0] = 0;
    if (outLen) *outLen = n > 0 ? n - 1 : 0;
    return r;
}

WCHAR *utf8_to_utf16(const char *s, int len)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, len, NULL, 0);
    WCHAR *r = (WCHAR *)malloc(((size_t)n + 1) * sizeof(WCHAR));
    if (!r) return NULL;
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, len, r, n);
    r[n > 0 ? n : 0] = 0;
    return r;
}

static char *read_all(const WCHAR *path)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    DWORD  n, got = 0;
    char  *p;
    if (f == INVALID_HANDLE_VALUE) return NULL;
    n = GetFileSize(f, NULL);
    if (n == INVALID_FILE_SIZE || n > (1 << 20)) { CloseHandle(f); return NULL; }
    p = (char *)malloc((size_t)n + 1);
    if (p && !ReadFile(f, p, n, &got, NULL)) got = 0;
    CloseHandle(f);
    if (!p) return NULL;
    p[got] = 0;
    return p;
}

static int pick(const char *v, const char *const *names, int n, int def)
{
    int i;
    for (i = 0; i < n; i++) if (!_stricmp(v, names[i])) return i;
    return def;
}

void config_load(void)
{
    char *text = read_all(g_iniPath), *line, *next;
    if (!text) return;
    for (line = text; line && *line; line = next) {
        char *eq, *key, *val, *e;
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        if ((e = strchr(line, '\r')) != NULL) *e = 0;
        if ((unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF) line += 3;
        while (*line == ' ' || *line == '\t') line++;
        if (*line == ';' || *line == '#' || *line == '[' || !*line) continue;
        eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        key = line;
        val = eq + 1;
        for (e = eq - 1; e >= key && (*e == ' ' || *e == '\t'); e--) *e = 0;
        while (*val == ' ' || *val == '\t') val++;
        for (e = val + strlen(val) - 1; e >= val && (*e == ' ' || *e == '\t'); e--) *e = 0;

        if (!_stricmp(key, "quality")) g_cfg.quality = pick(val, k_quality, Q_COUNT, Q_AUTO);
        else if (!_stricmp(key, "viewonly")) g_cfg.viewOnly = atoi(val) != 0;
        else if (!_stricmp(key, "fullscreen")) g_cfg.fullscreen = atoi(val) != 0;
        else if (!_stricmp(key, "fit")) g_cfg.fit = atoi(val) != 0;
        else if (!_stricmp(key, "grab")) g_cfg.grab = pick(val, k_grab, 3, GRAB_FULLSCREEN);
        else if (!_stricmp(key, "stats")) g_cfg.showStats = atoi(val) != 0;
        else if (!_stricmp(key, "nosleep")) g_cfg.noSleep = atoi(val) != 0;
        else if (!_stricmp(key, "render")) g_cfg.renderGdi = _stricmp(val, "gpu") != 0;
        else if (!_stricmp(key, "audio")) g_cfg.audio = atoi(val) != 0;
        else if (!_stricmp(key, "audioquality")) g_cfg.audioSpeed = !_stricmp(val, "speed");
        else if (!_stricmp(key, "theme")) g_cfg.theme = !_stricmp(val, "light") ? 1 : !_stricmp(val, "dark") ? 2 : 0;
        else if (!_stricmp(key, "log")) g_cfg.log = atoi(val) != 0;
        else if (!_stricmp(key, "host") && g_cfg.nhistory < MAX_HISTORY && *val)
            MultiByteToWideChar(CP_UTF8, 0, val, -1, g_cfg.history[g_cfg.nhistory++], 256);
        else if (!_strnicmp(key, "password.", 9) && g_npw < MAX_PW && !strncmp(val, "dpapi:", 6)) {
            MultiByteToWideChar(CP_UTF8, 0, key + 9, -1, g_pw[g_npw].host, 256);
            g_pw[g_npw].hex = _strdup(val + 6);
            if (g_pw[g_npw].hex) g_npw++;
        }
    }
    free(text);
}

BOOL config_save(void)
{
    char  *buf = (char *)malloc(65536), *p;
    WCHAR  tmp[MAX_PATH + 8];
    HANDLE f;
    DWORD  wr;
    BOOL   ok;
    int    i, n;

    if (!buf) return FALSE;
    p = buf;
    p += sprintf(p,
        "; iiv-client の設定(UTF-8)\n"
        "[client]\n"
        "; 画質(求めるビットレート): auto(サーバーに任せる。おすすめ)/ lan(多め。LAN 向け)/ wifi(Wi-Fi・遠隔地)/ slow(細い回線)\n"
        "quality=%s\n"
        "; 1 = 見るだけ(キー・マウスを送らない)\n"
        "viewonly=%d\n"
        "; 1 = 全画面で開く\n"
        "fullscreen=%d\n"
        "; 1 = 窓に合わせて縮める、0 = 等倍\n"
        "fit=%d\n"
        "; Windows キーや Alt+Tab を相手へ送るとき: fullscreen(全画面のとき)/ always / never\n"
        "grab=%s\n"
        "; 1 = タイトルに更新の速さを出す\n"
        "stats=%d\n"
        "; 描画: gpu(既定。D3D11。縮めても文字がきれい)/ gdi(メモリが少ない。縮めると遅く粗い)\n"
        "render=%s\n"
        "; 1 = つないでいる間はスリープさせず、画面も消さない\n"
        "nosleep=%d\n"
        "; 1 = 音を鳴らす(サーバーも「音を鳴らす」にしていれば、サーバーで鳴っている音をこちらで鳴らす)\n"
        "audio=%d\n"
        "; 音質: quality(音質優先。そのまま送る。1.5Mbps)/ speed(速度優先。AAC で 0.1Mbps。音質が少し落ち、70〜90ms 遅れる)\n"
        "audioquality=%s\n"
        "\n[history]\n",
        k_quality[g_cfg.quality], g_cfg.viewOnly, g_cfg.fullscreen, g_cfg.fit, k_grab[g_cfg.grab], g_cfg.showStats,
        g_cfg.renderGdi ? "gdi" : "gpu", g_cfg.noSleep, g_cfg.audio, g_cfg.audioSpeed ? "speed" : "quality");
    for (i = 0; i < g_cfg.nhistory; i++) {
        char *h = utf16_to_utf8(g_cfg.history[i], NULL);
        if (h) { p += sprintf(p, "host=%s\n", h); free(h); }
    }
    p += sprintf(p, "\n[passwords]\n; 「パスワードを覚える」にした接続先(Windows の DPAPI で暗号にしたもの。この PC のこの利用者だけが戻せる)\n");
    for (i = 0; i < g_npw; i++) {
        char *h = utf16_to_utf8(g_pw[i].host, NULL);
        if (h) { p += sprintf(p, "password.%s=dpapi:%s\n", h, g_pw[i].hex); free(h); }
    }
    p += sprintf(p, "\n[general]\n; system / light / dark\ntheme=%s\n; 1 = iiv-client.log に動作の記録を書く\nlog=%d\n",
                 g_cfg.theme == 1 ? "light" : g_cfg.theme == 2 ? "dark" : "system", g_cfg.log);
    n = (int)(p - buf);

    wsprintfW(tmp, L"%s.tmp", g_iniPath);
    f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { free(buf); return FALSE; }
    ok = WriteFile(f, buf, (DWORD)n, &wr, NULL) && wr == (DWORD)n;
    ok = FlushFileBuffers(f) && ok;
    CloseHandle(f);
    free(buf);
    if (ok) ok = MoveFileExW(tmp, g_iniPath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) DeleteFileW(tmp);
    return ok;
}

void config_add_history(const WCHAR *host)
{
    int i, j;
    if (!host[0]) return;
    for (i = 0; i < g_cfg.nhistory; i++) if (!lstrcmpiW(g_cfg.history[i], host)) break;
    if (i == g_cfg.nhistory) {
        if (g_cfg.nhistory < MAX_HISTORY) g_cfg.nhistory++;
        i = g_cfg.nhistory - 1;
    }
    for (j = i; j > 0; j--) lstrcpyW(g_cfg.history[j], g_cfg.history[j - 1]);
    lstrcpynW(g_cfg.history[0], host, 256);
}

BOOL config_saved_password(const WCHAR *host, char *out, int cap)
{
    int i;
    out[0] = 0;
    for (i = 0; i < g_npw; i++) {
        size_t    n = strlen(g_pw[i].hex) / 2, k;
        BYTE     *b;
        DATA_BLOB in, plain;
        BOOL      ok = FALSE;
        if (lstrcmpiW(g_pw[i].host, host)) continue;
        b = (BYTE *)malloc(n + 1);
        if (!b) return FALSE;
        for (k = 0; k < n; k++) {
            unsigned v;
            if (sscanf(g_pw[i].hex + k * 2, "%2x", &v) != 1) break;
            b[k] = (BYTE)v;
        }
        in.pbData = b;
        in.cbData = (DWORD)k;
        if (k == n && CryptUnprotectData(&in, NULL, NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &plain)) {
            if ((int)plain.cbData < cap) {
                memcpy(out, plain.pbData, plain.cbData);
                out[plain.cbData] = 0;
                ok = TRUE;
            }
            SecureZeroMemory(plain.pbData, plain.cbData);
            LocalFree(plain.pbData);
        }
        free(b);
        return ok;
    }
    return FALSE;
}

void config_set_password(const WCHAR *host, const char *pw)
{
    DATA_BLOB in, enc;
    int i;
    DWORD k;
    for (i = 0; i < g_npw; i++) if (!lstrcmpiW(g_pw[i].host, host)) break;
    if (!pw || !pw[0]) {
        if (i < g_npw) { free(g_pw[i].hex); g_pw[i] = g_pw[--g_npw]; }
        return;
    }
    in.pbData = (BYTE *)pw;
    in.cbData = (DWORD)strlen(pw);
    if (!CryptProtectData(&in, L"iiv-client", NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &enc)) return;
    if (i == g_npw) {
        if (g_npw >= MAX_PW) { LocalFree(enc.pbData); return; }
        g_pw[g_npw].hex = NULL;
        g_npw++;
    }
    lstrcpynW(g_pw[i].host, host, 256);
    free(g_pw[i].hex);
    g_pw[i].hex = (char *)malloc((size_t)enc.cbData * 2 + 1);
    if (g_pw[i].hex) {
        for (k = 0; k < enc.cbData; k++) sprintf(g_pw[i].hex + k * 2, "%02x", enc.pbData[k]);
        g_pw[i].hex[enc.cbData * 2] = 0;
    } else {
        g_pw[i] = g_pw[--g_npw];
    }
    LocalFree(enc.pbData);
}

void log_open(void)
{
    WCHAR path[MAX_PATH], *p;
    if (!g_cfg.log || g_logFile != INVALID_HANDLE_VALUE) return;
    lstrcpynW(path, g_iniPath, MAX_PATH - 8);
    p = wcsrchr(path, L'.');
    if (p) *p = 0;
    lstrcatW(path, L".log");
    g_logFile = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
}

void log_printf(const WCHAR *fmt, ...)
{
    WCHAR   w[1024];
    char   *u, head[40];
    int     n;
    va_list ap;
    SYSTEMTIME t;
    DWORD wr;
    if (g_logFile == INVALID_HANDLE_VALUE) return;
    va_start(ap, fmt);
    _vsnwprintf(w, ARRAYSIZE(w) - 1, fmt, ap);
    va_end(ap);
    w[ARRAYSIZE(w) - 1] = 0;
    u = utf16_to_utf8(w, &n);
    if (!u) return;
    GetLocalTime(&t);
    sprintf(head, "%04d-%02d-%02d %02d:%02d:%02d.%03d ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    EnterCriticalSection(&g_logCs);
    WriteFile(g_logFile, head, (DWORD)strlen(head), &wr, NULL);
    WriteFile(g_logFile, u, (DWORD)n, &wr, NULL);
    WriteFile(g_logFile, "\r\n", 2, &wr, NULL);
    LeaveCriticalSection(&g_logCs);
    free(u);
}
