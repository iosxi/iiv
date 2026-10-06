/* ==================================================================
 * ui.c - 接続の画面と、お知らせ
 *
 *  サーバーの欄は前に使った接続先を覚えている(最大 16 件)。
 *  「パスワードを覚える」にすると、Windows の DPAPI で暗号にして ini に置く。
 *  接続先を選び直すと、覚えたパスワードを入れ直す。
 *
 *  左下の「ネットワークの許可を消す」: Windows ファイアウォールに、この exe の規則が
 *  あれば消す(管理者で。fwrules.c)。無ければ「ネットワークの許可: なし」で押せない。
 * ================================================================== */

#include "iivc.h"
#include "resource.h"

static const WCHAR *g_error;
static BOOL  g_info;            /* IDC_ERROR に出しているのがお知らせ(赤くしない) */
static HFONT g_heading;
static int   g_footerTop;

int ui_message(HWND owner, const WCHAR *main, const WCHAR *content, int buttons, PCWSTR icon)
{
    TASKDIALOGCONFIG  tc;
    TASKDIALOG_BUTTON b[2] = { { IDRETRY, L"再接続(&R)" }, { IDCANCEL, L"閉じる(&C)" } };
    int pressed = IDCANCEL;
    ZeroMemory(&tc, sizeof(tc));
    tc.cbSize = sizeof(tc);
    tc.hwndParent = owner;
    tc.hInstance = g_inst;
    tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW | TDF_SIZE_TO_CONTENT;
    tc.pszWindowTitle = APP_NAME;
    tc.pszMainIcon = icon;
    tc.pszMainInstruction = main;
    tc.pszContent = content;
    if (buttons == 1) {
        tc.pButtons = b;
        tc.cButtons = 2;
        tc.nDefaultButton = IDRETRY;
    } else {
        tc.dwCommonButtons = TDCBF_OK_BUTTON;
    }
    if (FAILED(TaskDialogIndirect(&tc, &pressed, NULL, NULL))) pressed = IDCANCEL;
    return pressed;
}

static void load_saved_password(HWND dlg)
{
    WCHAR host[256];
    char  pw[PW_MAX];
    GetDlgItemTextW(dlg, IDC_HOST, host, ARRAYSIZE(host));
    if (config_saved_password(host, pw, sizeof(pw))) {
        WCHAR w[80];
        MultiByteToWideChar(CP_UTF8, 0, pw, -1, w, ARRAYSIZE(w));
        SetDlgItemTextW(dlg, IDC_PASSWORD, w);
        CheckDlgButton(dlg, IDC_SAVEPW, BST_CHECKED);
        SecureZeroMemory(pw, sizeof(pw));
        SecureZeroMemory(w, sizeof(w));
    } else {
        SetDlgItemTextW(dlg, IDC_PASSWORD, L"");
        CheckDlgButton(dlg, IDC_SAVEPW, BST_UNCHECKED);
    }
}

static void fw_refresh(HWND dlg)
{
    FwInfo fi;
    HWND   b = GetDlgItem(dlg, IDC_FWREMOVE);
    BOOL   any = fw_query(NULL, &fi) && fi.count > 0;
    SetWindowTextW(b, any ? L"ネットワークの許可を消す(&N)..." : L"ネットワークの許可: なし");
    EnableWindow(b, any);
}

static void fw_remove_now(HWND dlg)
{
    WCHAR s[200];
    int   n = fw_remove_elevated(dlg, NULL, L"-remove-firewall");
    if (n < 0) lstrcpyW(s, L"ネットワークの許可を消せませんでした(管理者の確認を断ったか、失敗しました)。");
    else swprintf(s, ARRAYSIZE(s), L"Windows ファイアウォールから、この exe の許可設定を %d 件消しました。", n);
    g_info = TRUE;
    SetDlgItemTextW(dlg, IDC_ERROR, s);
    ShowWindow(GetDlgItem(dlg, IDC_ERROR), SW_SHOW);
    fw_refresh(dlg);
    if (!IsWindowEnabled(GetFocus())) SetFocus(GetDlgItem(dlg, IDOK));
}

static INT_PTR CALLBACK dlg_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        HWND     host = GetDlgItem(dlg, IDC_HOST), q = GetDlgItem(dlg, IDC_QUALITY);
        LOGFONTW lf;
        HFONT    base = (HFONT)SendMessageW(dlg, WM_GETFONT, 0, 0);
        RECT     r, pad = { 0, 0, 0, 7 };
        int      i;
        (void)lp;
        SendMessageW(dlg, WM_SETICON, ICON_SMALL, (LPARAM)LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                     GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
        SendMessageW(dlg, WM_SETICON, ICON_BIG, (LPARAM)LoadIconW(g_inst, MAKEINTRESOURCEW(IDI_APP)));
        if (base && GetObjectW(base, sizeof(lf), &lf)) {
            lf.lfWeight = FW_SEMIBOLD;
            lf.lfHeight = MulDiv(lf.lfHeight, 118, 100);
            g_heading = CreateFontIndirectW(&lf);
            SendDlgItemMessageW(dlg, IDC_H_CONNECT, WM_SETFONT, (WPARAM)g_heading, TRUE);
        }
        GetWindowRect(GetDlgItem(dlg, IDOK), &r);
        MapWindowPoints(NULL, dlg, (POINT *)&r, 2);
        MapDialogRect(dlg, &pad);
        g_footerTop = r.top - pad.bottom;
        SetPropW(dlg, L"iiv.footer", (HANDLE)(INT_PTR)g_footerTop);

        for (i = 0; i < g_cfg.nhistory; i++) SendMessageW(host, CB_ADDSTRING, 0, (LPARAM)g_cfg.history[i]);
        if (g_cfg.host[0]) SetWindowTextW(host, g_cfg.host);
        else if (g_cfg.nhistory) SetWindowTextW(host, g_cfg.history[0]);
        load_saved_password(dlg);
        if (g_cfg.password[0]) {
            WCHAR w[80];
            MultiByteToWideChar(CP_UTF8, 0, g_cfg.password, -1, w, ARRAYSIZE(w));
            SetDlgItemTextW(dlg, IDC_PASSWORD, w);
        }
        SendDlgItemMessageW(dlg, IDC_PASSWORD, EM_LIMITTEXT, 64, 0);
        SendMessageW(q, CB_ADDSTRING, 0, (LPARAM)L"自動(おすすめ)");
        SendMessageW(q, CB_ADDSTRING, 0, (LPARAM)L"高め(LAN 向け)");
        SendMessageW(q, CB_ADDSTRING, 0, (LPARAM)L"控えめ(Wi-Fi・遠隔地)");
        SendMessageW(q, CB_ADDSTRING, 0, (LPARAM)L"細い回線");
        SendMessageW(q, CB_SETCURSEL, (WPARAM)g_cfg.quality, 0);
        SendDlgItemMessageW(dlg, IDC_RENDER, CB_ADDSTRING, 0, (LPARAM)L"GDI(メモリが少ない)");
        SendDlgItemMessageW(dlg, IDC_RENDER, CB_ADDSTRING, 0, (LPARAM)L"GPU(おすすめ。縮めても文字がきれい)");
        SendDlgItemMessageW(dlg, IDC_RENDER, CB_SETCURSEL, g_cfg.renderGdi ? 0 : 1, 0);
        CheckDlgButton(dlg, IDC_VIEWONLY, g_cfg.viewOnly ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_FULLSCREEN, g_cfg.fullscreen ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_NOSLEEP, g_cfg.noSleep ? BST_CHECKED : BST_UNCHECKED);
        fw_refresh(dlg);
        g_info = FALSE;
        if (g_error) {
            SetDlgItemTextW(dlg, IDC_ERROR, g_error);
            ShowWindow(GetDlgItem(dlg, IDC_ERROR), SW_SHOW);
        } else {
            ShowWindow(GetDlgItem(dlg, IDC_ERROR), SW_HIDE);
        }
        theme_apply_dialog(dlg);
        if (g_error && (conn_needs_password() || conn_auth_failed())) {
            SetFocus(GetDlgItem(dlg, IDC_PASSWORD));
            return FALSE;
        }
        return TRUE;
    }

    case WM_ERASEBKGND: {
        HDC  dc = (HDC)wp;
        RECT c, f;
        GetClientRect(dlg, &c);
        f = c;
        c.bottom = g_footerTop;
        f.top = g_footerTop;
        FillRect(dc, &f, theme_footer_brush());
        {
            HBRUSH line = CreateSolidBrush(theme_line());
            RECT l = f;
            l.bottom = l.top + 1;
            FillRect(dc, &l, line);
            DeleteObject(line);
        }
        FillRect(dc, &c, theme_back_brush());
        SetWindowLongPtrW(dlg, DWLP_MSGRESULT, 1);
        return TRUE;
    }

    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        int id = GetDlgCtrlID((HWND)lp);
        LRESULT r;
        if (msg == WM_CTLCOLORSTATIC && id == IDC_ERROR && !g_info) {
            theme_ctlcolor(msg, (HDC)wp, (HWND)lp, FALSE);
            SetTextColor((HDC)wp, theme_is_dark() ? RGB(255, 153, 164) : RGB(196, 43, 28));
            return (INT_PTR)theme_back_brush();
        }
        r = theme_ctlcolor(msg, (HDC)wp, (HWND)lp, id == IDC_HINT);
        if (r) return (INT_PTR)r;
        break;
    }

    case WM_NOTIFY: {
        LRESULT res;
        if (((NMHDR *)lp)->code == NM_CUSTOMDRAW && theme_custom_draw_button((NMCUSTOMDRAW *)lp, &res)) {
            SetWindowLongPtrW(dlg, DWLP_MSGRESULT, res);
            return TRUE;
        }
        break;
    }

    case WM_COMMAND:
        if (LOWORD(wp) == IDC_HOST && HIWORD(wp) == CBN_SELCHANGE) {
            int sel = (int)SendDlgItemMessageW(dlg, IDC_HOST, CB_GETCURSEL, 0, 0);
            if (sel >= 0) {
                WCHAR h[256];
                SendDlgItemMessageW(dlg, IDC_HOST, CB_GETLBTEXT, (WPARAM)sel, (LPARAM)h);
                SetDlgItemTextW(dlg, IDC_HOST, h);
                load_saved_password(dlg);
            }
            return TRUE;
        }
        if (LOWORD(wp) == IDOK) {
            WCHAR host[256], pw[80], h2[256];
            int   port;
            GetDlgItemTextW(dlg, IDC_HOST, host, ARRAYSIZE(host));
            if (!conn_parse_host(host, h2, ARRAYSIZE(h2), &port)) {
                g_info = FALSE;
                SetDlgItemTextW(dlg, IDC_ERROR, L"サーバーを入れてください。例: 192.168.1.10、pc-name、host:5960");
                ShowWindow(GetDlgItem(dlg, IDC_ERROR), SW_SHOW);
                SetFocus(GetDlgItem(dlg, IDC_HOST));
                return TRUE;
            }
            GetDlgItemTextW(dlg, IDC_PASSWORD, pw, ARRAYSIZE(pw));
            lstrcpynW(g_cfg.host, host, ARRAYSIZE(g_cfg.host));
            WideCharToMultiByte(CP_UTF8, 0, pw, -1, g_cfg.password, sizeof(g_cfg.password), NULL, NULL);
            g_cfg.password[sizeof(g_cfg.password) - 1] = 0;
            SecureZeroMemory(pw, sizeof(pw));
            g_cfg.savePassword = IsDlgButtonChecked(dlg, IDC_SAVEPW) == BST_CHECKED;
            g_cfg.quality = (int)SendDlgItemMessageW(dlg, IDC_QUALITY, CB_GETCURSEL, 0, 0);
            if (g_cfg.quality < 0 || g_cfg.quality >= Q_COUNT) g_cfg.quality = Q_AUTO;
            g_cfg.renderGdi = SendDlgItemMessageW(dlg, IDC_RENDER, CB_GETCURSEL, 0, 0) != 1;
            g_cfg.viewOnly = IsDlgButtonChecked(dlg, IDC_VIEWONLY) == BST_CHECKED;
            g_cfg.fullscreen = IsDlgButtonChecked(dlg, IDC_FULLSCREEN) == BST_CHECKED;
            g_cfg.noSleep = IsDlgButtonChecked(dlg, IDC_NOSLEEP) == BST_CHECKED;
            config_add_history(host);
            config_set_password(host, g_cfg.savePassword ? g_cfg.password : "");
            config_save();
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDC_FWREMOVE) {
            fw_remove_now(dlg);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;

    case WM_DESTROY:
        if (g_heading) DeleteObject(g_heading);
        g_heading = NULL;
        RemovePropW(dlg, L"iiv.footer");
        break;
    }
    return FALSE;
}

BOOL ui_connect_dialog(HWND owner, const WCHAR *error)
{
    g_error = error;
    return DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_CONNECT), owner, dlg_proc, 0) == IDOK;
}
