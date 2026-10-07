# iiv-server の作業方針

## このディレクトリについて

リリース・版・`common/` の扱いはルートの `../CLAUDE.md` に書いてある。ここには iiv-server 固有のことだけを書く。

- `src/iiv.h` の `APP_RELEASE` は設定画面のタイトルに出る。ファイルの版は `src/iiv-server.rc` の VERSIONINFO、
  `src/iiv-server.manifest` の `assemblyIdentity`、`src/iiv.h` の `APP_VERSION` / `APP_VERSION_A`。
- 前身は `../../iivnc-server`(VNC のサーバー。そのまま残す)。iiv は 2026-10-06 に VNC を捨てて作り始めた。

### exe を変更したとき

ソースを直したら **`build.bat` で exe を作り直してからコミットする**(ルートの `build.bat` なら両方)。
アイコンは `python tools/make-icon.py`(Pillow で 1024px に描いて縮める)。サーバーは青いタワー型 PC と外へ出る矢印、接続中(`-active.ico`)は電源ボタンと矢印の縁が緑。クライアントは緑のノート PC と画面へ入る矢印。
Pillow の `polygon(..., outline=, width=)` は縁を内側にしか描かない(白い矢印が細くなる)。縁は `line(..., joint="curve")` で
輪郭をなぞってから塗りを重ねる。

## 動作確認について

- **`python tools/test.py`**: サーバーを `-testsrc`(合成した絵。入力はログに書くだけ)と検証用の ini
  (`build/test/test.ini`、127.0.0.1:5999)で動かし、`tools/iivcheck.py`(Python の受け手)で認証・
  動く絵 300 フレーム(`build/mf/mfdec.exe` で全部復号できるか)・止まった絵・大きさの変更・3 人同時・
  返事を返さない相手・でたらめなデータを確かめる。`tools/build-tools.bat` が `tools/mf*.c` を `build/mf` に作る。
- **`python tools/realbench.py --scene office|video|idle [--ini "qmove=70;..."]`**: 実画面(DXGI)を `-dryrun` と
  127.0.0.1:5913 で取り込み、iivnc の `../../iivnc-server/tools/srcwin.py` の窓を出して、フレーム/秒・帯域・遅れ・CPU を測る。
- クライアントと組み合わせた検証は `../client/tools/clientcheck.py`、速さは `../client/tools/pairbench.py`。
- 試験プログラム: `tools/mfprobe.c`(使えるエンコーダ・デコーダの一覧)、`tools/mfenc.c`(NV12 の連番を符号化。
  `cbr|qp|qp2|qps|q|qdyn`)、`tools/mfdec.c`(復号して PSNR、最後のフレームを書き出す)。

## 実測で分かったこと(2026-10-06、i7-12700KF・RTX 3070 Ti・Windows 11・4K 60Hz)

- 使える部品(`mfprobe`): H.264 のエンコーダは NVIDIA H.264 Encoder MFT(GPU)・Microsoft AVC DX12 Encoder・
  H264 Encoder MFT(CPU)。HEVC は NVIDIA のみ。**デコーダは H.264 だけが Windows 標準**(HEVC・AV1 はストアの拡張)。
  → Windows 10 で追加のインストールなしに動かすため H.264 にした。
- **NVIDIA の MFT は `CODECAPI_AVEncVideoEncodeQP` を無視する**(読み返すと設定した値なのに、16 でも 30 でも同じ大きさ。
  フレームごとの `MFSampleExtension_VideoEncodeQP` も効かない)。**`CODECAPI_AVEncCommonQuality`(0〜100)は効く**
  (1080p の文字の絵で 30 → PSNR 34.2dB、90 → 50.2dB)。**符号化の途中で変えても次のフレームから効く**
  (画質 40 の止まった絵に 95 を 1 枚: 36.95 → 49.71dB、その 1 枚 125KB、以後 113 バイト)。
- CBR は止まった画面でも枠を使い切る(4K・80Mbps の枠で、縞しか変わらない絵に 73Mbps)。上限付き可変でも 54Mbps。
  画質で決めると 0.4〜7Mbps。
- 符号化の時間は画素数に比例: 1080p 2.5ms、4K 9〜11ms。`CODECAPI_AVEncCommonQualityVsSpeed`=0 で 4K 9.2ms。
- NVIDIA の MFT は B フレームの数の設定を受け付けない(0x80070057)が、低遅延モードで 1 枚入れると 1 枚出る。
- 非同期 MFT のイベントは `IMFAsyncCallback`(C で vtable を手書き)で受け、Win32 のイベントで待つ(空回りしない)。
- 色の変換は VideoProcessor(入力 RGB 0〜255、出力 BT.709 16〜235)。クライアントで戻した絵は背景の灰色・色の帯が一致。
  明るさの PSNR は止まった絵で 46dB だが、RGB では 30dB(色差 4:2:0 で ClearType の色付きの縁が崩れる)。
- 返事を返さない相手: 待ち行列があふれたら捨てて `resync`。送り手が追いつくまでキーフレームを求めない
  (すぐ求めると、受け取らない相手のためにキーフレームを作り続け、ほかの相手が 11 フレーム/秒に落ちた)。
- 検証用の絵の速さは `Sleep` で合わせるので、`timeBeginPeriod(1)` が要る(既定の 15.6ms 刻みでは 32fps だった)。

### 音を鳴らす(audio.c。2026-10-07、`tools/audioprobe.c` で下調べ)

- ループバックの形はミックスのまま(この PC は 48kHz・32bit float・2ch)。**何も鳴っていないとパケットは 1 つも来ない**
  (イベントでも 10ms ごとの読み取りでも)。何かが鳴っていれば(音量 0 の再生でも)10ms ごとに来る。
- イベントで待つ形(`EVENTCALLBACK`)は、パケットの間が最大 112ms 空いた。10ms ごとに読むと最大 24ms → 10ms ごとに読む
  (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`。無ければ `timeBeginPeriod(1)`)。
- ループバックで `AUTOCONVERTPCM` に 16bit・48kHz を頼むと、初期化は通るが 2 秒で 1/4 しか来なかった → 変換は自前。
- ループバックの `pu64QPCPosition` は「サーバーのスピーカーで鳴る時刻」らしい(届いた時刻より 3ms 先だった)。
- AAC(Microsoft AAC Audio Encoder MFT): 入力の形を先に決める。96kbps と 128kbps で SNR はほぼ同じ(52.4 / 53.1dB)。
  最初のフレームは 3360 サンプル入れてから出る(70ms の遅れ)。低遅延モードは無い(`CODECAPI_AVLowLatencyMode` が E_NOTIMPL)。
  音が途切れたら、残った分を無音で押し出す(再開したときに古い音が出ないように)。
- 音を求める相手がいなければ取り込みのスレッドを起こさない。書き手の `send_audio` も、使っていなければロックも取らずに戻る。
  `pairbench.py` で「鳴らさない」58.3〜58.9fps、「音質優先」59.1〜59.3fps、「速度優先」59.0〜59.1fps(3 回ずつ)。

## 確かめていないこと

- Windows 10 の機械、Intel / AMD の GPU、CPU のエンコーダだけの PC、サービス(svc.c)、クリップボード、
  ファイルのコピー＆貼り付け、リモート デスクトップの中、2 台の実機の間、複数の画面。
- 音: 実際に鳴っている音を耳で聞くこと、サービス(SYSTEM の分身)からのループバック、多チャンネル・44.1/48kHz 以外の
  再生デバイス(変換は書いたが動かしていない)、既定の再生デバイスの切り替え。
