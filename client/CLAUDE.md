# iiv-client の作業方針

## このディレクトリについて

リリース・版・`common/` の扱いはルートの `../CLAUDE.md` に書いてある。ここには iiv-client 固有のことだけを書く。

- `src/iivc.h` の `APP_RELEASE` は接続の画面と表示の窓のタイトルに出る。ファイルの版は `src/iiv-client.rc` の
  VERSIONINFO、`src/iiv-client.manifest` の `assemblyIdentity`、`src/iivc.h` の `APP_VERSION`。
- 前身は `../../iivnc-client`(VNC のビューア。そのまま残す)。

### exe を変更したとき

ソースを直したら **`build.bat` で exe を作り直してからコミットする**(ルートの `build.bat` なら両方)。
`build.bat` は `fxc` で `src/view.hlsl` の `vs`・`ps`・`ps_nv12` をバイト列(`build/shader_*.h`)にして埋め込む。
アイコンは `python tools/make-icon.py`(Pillow で 1024px に描いて縮める)。サーバーは青いタワー型 PC と外へ出る矢印、接続中(`-active.ico`)は電源ボタンと矢印の縁が緑。クライアントは緑のノート PC と画面へ入る矢印。
Pillow の `polygon(..., outline=, width=)` は縁を内側にしか描かない(白い矢印が細くなる)。縁は `line(..., joint="curve")` で
輪郭をなぞってから塗りを重ねる。

## 動作確認について

検証には `../server` の exe と `../server/tools/iivcheck.py`(起動・停止)を使う。サーバーは `-testsrc`(合成した絵。
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
- 入力の検証のマウスは「ボタンを押す直前の移動」で判定する。本物のカーソルが新しく出た窓に重なっていると、
  その位置への移動が先にサーバーへ届く(2026-10-06 に (1244,742) と出て気づいた。iivnc でも同じだった)。
- 止まった検証用の絵: 明るさの PSNR は GPU 46.0dB(文字の欄 40.9)、GDI 44.0dB(39.0)。RGB では 30dB 前後
  (色差 4:2:0 で ClearType の色付きの縁が崩れる。差の画像で文字の縁だけに出ることを確かめた)。
- パスワードは BCrypt の PBKDF2-SHA256 でサーバーのソルト 2 つ(操作用・見るだけ用)から鍵を作り、HMAC で答える。
  覚えたパスワードは DPAPI(`CryptProtectData`)で暗号にして ini に置く。

### 音を鳴らす(audio.c。2026-10-07)

- 通信のスレッドは音を写して待ち行列に入れるだけ。復号と再生は audio.c のスレッド(MMCSS "Pro Audio")。
- 再生は共有モードで、こちらの形(16bit ステレオ)のまま `AUTOCONVERTPCM | SRC_DEFAULT_QUALITY | RATEADJUST`。
  44.1kHz・48kHz とも初期化でき、`IAudioClockAdjustment::SetSampleRate` で +0.5% にすると、その分速く減った。
- **溜まり具合は「輪 + デバイス」の合計で見る。** 目標を輪だけで 30ms にしたら、20ms がすぐデバイスへ移って輪に 10ms しか
  残らず、届く間隔の揺れ(13〜17ms)で途切れ、速さの調整も −0.5% に張り付いた。今は PCM 50ms・AAC 70ms。
- デバイスを開くのに 157〜238ms かかり、その間に届いた分が溜まる → 鳴らし始めるときに目標まで切り詰める。
- つないだ直後、通信のスレッドが映像のデコーダを開く間(31ms)音が届かない。目標 50ms なら途切れない。
- AAC は「届くまで」が 70ms(エンコーダの遅れ)。サンプルの位置はずれない(audiocheck.py で確かめた)。

## 確かめていないこと

- Windows 10 の機械、Intel / AMD の GPU、クリップボード、ファイルのコピー＆貼り付け、2 台の実機の間、
  描画の方式をつないだまま切り替えること、全画面とキーの横取り(iivnc-client から引き継いだだけ)。
- 音: 耳で聞くこと、2 台の間(時計のずれの吸収)、再生デバイスを途中で切り替える・抜くこと、
  `RATEADJUST` が使えない PC(サンプルを落とす・重ねる側は動かしていない)。
