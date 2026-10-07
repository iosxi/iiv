/* ==================================================================
 * fxpaste.c - クリップボードのファイルを、エクスプローラーの貼り付けと同じように
 *             貼り付け先フォルダの IDropTarget へ落とす(tools/fxbench.py の --explorer が使う)
 *
 *    fxpaste.exe <貼り付け先のフォルダ> [待つ秒数(既定 600)]
 *
 *  iiv が置いた「中身はあとから届くファイル」(CFSTR_FILEDESCRIPTORW / CFSTR_FILECONTENTS)を、
 *  エクスプローラーと同じように 1 つずつ読ませる。落とした後は、メッセージを回しながら
 *  待つ(終わりは呼び手が貼り付け先を見て決め、このプロセスを終わらせる)。
 *
 *  2026-10-07 に試して行き止まりだったもの:
 *  - PowerShell からシェルの「貼り付け」(InvokeVerb('Paste')): フォルダを作ったところで止まった。
 *  - IFileOperation::CopyItems に IDataObject を渡す: DV_E_FORMATETC(仮想のファイルは扱えない)。
 * ================================================================== */
#define COBJMACROS
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <stdio.h>
#include <stdlib.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

int wmain(int argc, WCHAR **argv)
{
    IDataObject *data = NULL;
    IShellItem  *dest = NULL;
    IDropTarget *dt = NULL;
    HRESULT      hr;
    DWORD        effect = DROPEFFECT_COPY, waitMs, t0;
    POINTL       pt = { 0, 0 };

    if (argc < 2) { fprintf(stderr, "usage: fxpaste.exe <dest folder> [seconds]\n"); return 2; }
    waitMs = (argc > 2 ? (DWORD)_wtoi(argv[2]) : 600) * 1000;
    hr = OleInitialize(NULL);
    if (SUCCEEDED(hr)) hr = OleGetClipboard(&data);
    if (SUCCEEDED(hr)) hr = SHCreateItemFromParsingName(argv[1], NULL, &IID_IShellItem, (void **)&dest);
    if (SUCCEEDED(hr)) hr = IShellItem_BindToHandler(dest, NULL, &BHID_SFUIObject, &IID_IDropTarget, (void **)&dt);
    if (SUCCEEDED(hr)) hr = IDropTarget_DragEnter(dt, data, MK_LBUTTON, pt, &effect);
    if (SUCCEEDED(hr)) {
        effect = DROPEFFECT_COPY;
        hr = IDropTarget_Drop(dt, data, MK_LBUTTON, pt, &effect);
    }
    if (FAILED(hr)) { fprintf(stderr, "paste failed (0x%08lX)\n", hr); return 1; }
    printf("dropped (effect %lu)\n", effect);
    fflush(stdout);
    /* コピーはこのスレッドのメッセージを使うことがある。回しながら待つ */
    t0 = GetTickCount();
    while (GetTickCount() - t0 < waitMs) {
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        MsgWaitForMultipleObjects(0, NULL, FALSE, 50, QS_ALLINPUT);
    }
    IDropTarget_Release(dt);
    IShellItem_Release(dest);
    IDataObject_Release(data);
    OleUninitialize();
    return 0;
}
