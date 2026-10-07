# iiv の作業方針

iiv は Windows 10 / 11 用のリモート デスクトップ。サーバー(`server/`、iiv-server.exe)とクライアント
(`client/`、iiv-client.exe)を 1 つのリポジトリで扱う。それぞれ固有のことは `server/CLAUDE.md` と
`client/CLAUDE.md` に書く。

```
common/   両方が使うファイル(通信の取り決め・圧縮・配色・ファイアウォール・ファイルのコピー・AAC・WASAPI の ID)
server/   iiv-server(src/、tools/、build.bat、iiv-server.exe)
client/   iiv-client(src/、tools/、build.bat、iiv-client.exe)
build.bat 両方をビルドする(server\build.bat と client\build.bat を順に呼ぶ)
```

2026-10-07 までは `iosxi/iiv-server` と `iosxi/iiv-client` の 2 つのリポジトリだった。履歴は `server/` と `client/` の
下へ移して取り込んである。古いタグ(サーバー v1〜v3、クライアント v1〜v4)は元のリポジトリにだけある。

## リリース運用

**手順は共通の `~/.claude/CLAUDE.md`「修正が終わったら、リリースまで通す」に従う。**
ここにはこのリポジトリ固有の事情だけを書く。

- リモート: `https://github.com/iosxi/iiv.git`(`iosxi/iiv`)
- ブランチ: **`master`**
- 最新バージョンの確認: `git tag --sort=-v:refname | head -1`
- **版はサーバーとクライアントで 1 つ。** 片方しか変わっていなくても、リリースには
  **`server/iiv-server.exe` と `client/iiv-client.exe` の両方**を付ける(同じ vN 同士が組み合わせられる、とする)。
  改名せず、そのまま `gh release create` に渡す。
- `server/src/iiv.h` と `client/src/iivc.h` の `APP_RELEASE`(L"vN")はタイトルに出る。
  **リリースのたびに両方をタグと同じ vN にする。**
- それとは別に、ファイルの版(`APP_VERSION`、`src/*.rc` の VERSIONINFO、`src/*.manifest` の `assemblyIdentity`)が
  それぞれにある。その exe の機能が変わったら上げる。
- 通信の取り決めを変えたら `common/iivproto.h` の `IIV_VERSION` を上げ、サーバーとクライアントを同じコミットで直す。

### exe を変更したとき

ソースを直したら **`build.bat`(ルート。両方を作る)で exe を作り直してからコミットする**。exe はリポジトリに追跡させている。
片方だけなら `server\build.bat` か `client\build.bat` でもよい。

### common/ のファイル

`iivproto.h`(通信の取り決め)、`zlite.h` `zdeflate.c` `zinflate.c`、`theme.c` `fwrules.c` `filexfer.c`、
`aac.c` `aac.h`(「音を鳴らす」の速度優先。符号化はサーバー、復号はクライアントが使う)、
`wasapi.h`(WASAPI の ID。C では実体がどの .lib にも無いので、各 exe の audio.c がこれを読んで定義する)。
両方の `build.bat` が `/Isrc /I..\common` を付けて `..\common\*.c` をコンパイルする。
`theme.c` `fwrules.c` `filexfer.c` `aac.c` は `#include "app.h"` で、その exe のヘッダーを読む
(`server/src/app.h` は `iiv.h`、`client/src/app.h` は `iivc.h`)。だから、この 3 つが使う関数や定数
(`APP_NAME`、`log_printf`、`fx_host_send` など)は両方のヘッダーにそろえておく。
**common/ を直したら両方をビルドし、両方の検証を通す。**

`zlite.h` `zdeflate.c` `zinflate.c` は **input-mouser(`C:\projects\windows\input-mouser\src`)にも同じものを写している**
(2026-10-07、input-mouser v13 のファイルの圧縮)。input-mouser は gcc(MinGW)でビルドするので、MSVC 専用の書き方は
`zlite.h` の `ZL_*`(`ZLITE_INTERNAL` のときだけ定義)で両方に通す。直したら input-mouser へも写し、両方でビルドして確かめる。
`filexfer.c` のまとめ読みと圧縮も、input-mouser の `filecopy.c` に同じ仕組みがある(送り方の関数と、1 束の上限 `PF_RAW_MAX` が違う)。

## 動作確認について

- サーバー単体: `cd server && python tools/test.py`
- 組み合わせ: `cd client && python tools/clientcheck.py`(`../server/iiv-server.exe` と `../server/tools/iivcheck.py` を使う)
- 組み合わせの速さ: `cd client && python tools/pairbench.py [--audio off|quality|speed]`
- 音を鳴らす: `cd client && python tools/audiocheck.py [--seconds 15]`(音は出さない。PCM は 1 サンプルずつ一致、
  AAC は SNR、両方とも欠け・途切れ、片方が切られているとき、つないだままの切り替え)
- ファイルの受け渡しの速さと中身の一致: `cd server && python tools/fxbench.py --explorer [--old <古い版の exe の置き場所>] [--files 1000 | --big 10] [--rtt 40] [--mbps 20]`。
  遅い回線をまねる中継を挟み、本物の iiv-client が置いた一覧を `tools/fxpaste.c`(エクスプローラーの貼り付けと同じく
  貼り付け先の IDropTarget へ落とす)で貼り付ける。先に `server/tools/build-tools.bat` で fxpaste.exe を作る。
  古い版は `git show v7:server/iiv-server.exe > build/v7/iiv-server.exe` のように取り出す。クリップボードを使う
  (元の文字は戻す)。PowerShell の InvokeVerb('Paste') と IFileOperation::CopyItems は仮想のファイルを貼り付けられなかった。
- 音の部品の下調べ: `server/tools/audioprobe.c`(`tools/build-tools.bat` で `server/build/mf/audioprobe.exe`。
  `loop` / `render` / `aac`)
- Python の出力は CP932 になるので、Git Bash から読むときは `PYTHONIOENCODING=utf-8` を付ける。
- 検証は `-ini`(`build/test/` の検証用 ini)とポート 5999 で動かすので、利用者が普段使いで動かしている
  iiv-server(ポート 5960)とはぶつからない。
- `.bat` は CRLF(ASCII だけ)。Git Bash の here-doc や sed でバックスラッシュを含む行を書き換えると壊れやすい。
  Python のスクリプトで直すほうが確か。**そのスクリプトも Write ツールでファイルに書いてから実行する。**
  here-doc や `python -c` で渡すと、`\a` がベル文字に、`\\n` が改行に化けた(2026-10-07 に 3 回。build-tools.bat と C のソースが壊れた)。
