# iiv-client の作業方針

## リリース運用

**手順は共通の `~/.claude/CLAUDE.md`「修正が終わったら、リリースまで通す」に従う。**
ここにはこのリポジトリ固有の事情だけを書く。

- リモート: `https://github.com/iosxi/iiv-client.git`(`iosxi/iiv-client`)
- ブランチ: **`master`**
- 最新バージョンの確認: `git tag --sort=-v:refname | head -1`
- リリースの添付物: **`iiv-client.exe`**。改名せず、そのまま `gh release create` に渡す。
- バージョン: タグの `vN` とは別に、`src/iiv-client.rc` の VERSIONINFO、`src/iiv-client.manifest` の
  `assemblyIdentity`、`src/iivc.h` の `APP_VERSION` がある。機能が変わったら全部上げる。
- 前身は `../iivnc-client`(VNC のビューア。そのまま残す)。

### exe を変更したとき

ソースを直したら **`build.bat` で exe を作り直してからコミットする**。exe はリポジトリに追跡させている。
`build.bat` は `fxc` で `src/view.hlsl` の `vs`・`ps`・`ps_nv12` をバイト列(`build/shader_*.h`)にして埋め込む。

### iiv-server と同じファイル

`src/iivproto.h`(通信の取り決め)、`src/zlite.h` `src/zdeflate.c` `src/zinflate.c`、
`src/theme.c` `src/fwrules.c` `src/filexfer.c`(この 3 つは先頭の `#include` だけ違う)は `../iiv-server/src` と
中身をそろえる。

## 動作確認について

検証には `../iiv-server` の exe と `tools/iivcheck.py`(起動・停止)を使う。サーバーは `-testsrc`(合成した絵。
入力はログに書くだけ)で動かすので、利用者の画面は写さず、入力も再現しない。

- **`python tools/clientcheck.py`**: 止まった絵(サーバーが `-testdump` で書いた元の絵と、`-dump` で書いた絵の
  明るさの PSNR。GPU と GDI)、動く絵 300 フレーム、途中の大きさの変更、キー・マウス(`PostMessage` で窓へ直接送り、
  サーバーのログの `[dryrun-key]` `[dryrun] mouse` と比べる)。検証用の引数を付けたときは窓を前面に出さない。
- **`python tools/pairbench.py [--src video|static|move] [--render gpu|gdi]`**: 組み合わせた速さ・CPU・遅れ。

## 仕組みと実測(2026-10-06、i7-12700KF・RTX 3070 Ti・Windows 11)

- 復号は Windows 標準の Microsoft H264 Video Decoder MFT(低遅延モード)。通信のスレッドが映像の設定を受けたら、
  `SendMessageTimeout(WM_APP_D3D)` で窓のスレッドに D3D11 のデバイス(動画の機能付き・マルチスレッド保護)を用意して
  もらい、`MFT_MESSAGE_SET_D3D_MANAGER` で渡す(DXVA)。出てきたテクスチャの配列の 1 枚を `view_submit_nv12` で
  自前の NV12 テクスチャへ写し、窓のスレッドが `ps_nv12`(BT.709 16〜235)で `g_tex` へ描いてから、今までどおり
  ミップマップで縮めて描く。GDI のときと GPU が使えないときは CPU で復号し、SSE2 で BGRX にして `g_rm.fb` に書く。
- 描画の方式を変えたら `conn_reset_decoder` で復号器を作り直し、キーフレームを求める。
- CPU: GPU で復号 1 フレーム 1.9ms、CPU で復号 13.5ms(うち復号 約 7ms)。
- 「表示した」の返事(`IIV_C_ACK`)は描いた後に返す。サーバーは返事の無いフレームが 2 つを超えると待つので、
  表示が 60Hz に合わせて待つ分、フレーム/秒は 60 で止まる。
- 止まった検証用の絵: 明るさの PSNR は GPU 46.0dB(文字の欄 40.9)、GDI 44.0dB(39.0)。RGB では 30dB 前後
  (色差 4:2:0 で ClearType の色付きの縁が崩れる。差の画像で文字の縁だけに出ることを確かめた)。
- パスワードは BCrypt の PBKDF2-SHA256 でサーバーのソルト 2 つ(操作用・見るだけ用)から鍵を作り、HMAC で答える。
  覚えたパスワードは DPAPI(`CryptProtectData`)で暗号にして ini に置く。

## 確かめていないこと

- Windows 10 の機械、Intel / AMD の GPU、クリップボード、ファイルのコピー＆貼り付け、2 台の実機の間、
  描画の方式をつないだまま切り替えること、全画面とキーの横取り(iivnc-client から引き継いだだけ)。
