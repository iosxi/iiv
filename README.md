# iiv

Windows 10 / 11 用のリモート デスクトップです。画面を GPU の動画エンコーダ(H.264)で送り、受け手も GPU で復号して描きます。
[iivnc](https://github.com/iosxi/iivnc-server)(VNC)の後継で、VNC の規格は使いません。

| | 役目 | 説明 |
|---|---|---|
| <img src="server/src/iiv-server.png" width="32"> **[iiv-server](server/README.md)** | 見られる・操作される側の PC で動かす | 通知領域に常駐し、ポート 5960 で待ち受ける |
| <img src="client/src/iiv-client.png" width="32"> **[iiv-client](client/README.md)** | 見る・操作する側の PC で動かす | サーバーのアドレスとパスワードを入れて接続する |

- **速く、軽い。** 4K の画面を 60 フレーム/秒で送って、サーバーの CPU は 2〜5%、帯域は数 Mbps。
- **インストール不要。** exe を好きな場所に置いて起動するだけ。ランタイムも DLL も、追加のコーデックも要りません
  (Windows に最初からある Media Foundation を使います)。
- **レジストリを使わない。** 設定は exe と同じ場所の ini だけ。
- **音も鳴らせる(v6 から。既定はオフ)。** サーバーとクライアントの両方で「音を鳴らす」をオンにすると、
  サーバーで鳴っている音をクライアントで鳴らします。音質優先(そのまま。1.5Mbps)と速度優先(AAC。0.1Mbps)を選べます。
  オフのときは何も変わらず、オンでも映像のフレーム/秒は変わりませんでした。

**通信は暗号化していません。** パスワードは回線を通りませんが、画面とキー入力はそのまま流れます。
家や職場の LAN の中か、VPN の中で使ってください。

## 入手

[リリース](https://github.com/iosxi/iiv/releases) に `iiv-server.exe` と `iiv-client.exe` を並べて置いています。
**サーバーとクライアントは同じ版のものを組み合わせてください。**

v4 までは別々のリポジトリ([iiv-server](https://github.com/iosxi/iiv-server)、
[iiv-client](https://github.com/iosxi/iiv-client))で配っていました。v5 からはここで両方を配ります。

## 構成とビルド

```
common/   両方が使うソース(通信の取り決め、圧縮、配色、ファイアウォール、ファイルのコピー、AAC)
server/   iiv-server
client/   iiv-client
```

VS 2022 Build Tools(C/C++)があれば、ルートの `build.bat` で両方を作ります(`server\iiv-server.exe` と
`client\iiv-client.exe`)。片方だけなら `server\build.bat` か `client\build.bat`。
