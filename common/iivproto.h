/* ==================================================================
 * iivproto.h - iiv-server と iiv-client の間の取り決め(common/ に 1 つだけ置き、両方から使う)
 *
 *  VNC(RFB)は使わない。TCP の上で、数値はすべてリトル エンディアン。
 *
 *  つなぐとき
 *    C→S  IivHello        "IIV1"、版、能力
 *    S→C  IivChallenge    "IIV1"、版、認証の種類、ソルトと繰り返し回数(操作用・見るだけ用)、ノンス
 *    C→S  (認証があれば)  HMAC-SHA256(鍵, ノンス || "iiv-auth") を操作用・見るだけ用の 2 つ(64 バイト)
 *                          鍵 = PBKDF2-SHA256(パスワードの UTF-8, ソルト, 繰り返し回数, 32 バイト)
 *                          (入れたパスワードがどちらのものか分からないので、両方のソルトで作る。
 *                           見るだけ用が無ければ 2 つ目は 0 で埋める)
 *    S→C  IivWelcome      結果、見るだけか、サーバーの名前(UTF-8)
 *  その後はメッセージ: [u32 長さ(種類の 1 バイトを含む)][u8 種類][中身]
 *
 *  映像は押し出し式。サーバーは画面が変わるたびに H.264 の 1 フレーム(Annex B)を送る。
 *  クライアントは表示したフレームの番号を IIV_C_ACK で返す。サーバーは返事の無いフレームが
 *  IIV_MAX_INFLIGHT を超えたら次の符号化を待つ(その間の変化は次のフレームにまとまる)。
 *
 *  音(「音を鳴らす」。両方で有効にしたときだけ)
 *    C→S  IIV_C_AUDIO     ほしい方式(IIV_AUDIO_*。OFF = 要らない)。つないだ後と、変えたときに送る
 *    S→C  IIV_S_AUDIO_CONFIG  形と状態。サーバー側で切られていれば status で知らせ、何も送らない
 *    S→C  IIV_S_AUDIO     鳴っている音。PCM は 10ms ずつ、AAC は 1024 サンプルずつ
 *  サーバーは、音を求める相手が 1 人もいなければ取り込みを始めない(映像には何も足さない)。
 *  古い版の相手は、知らない種類のメッセージを読み飛ばす。
 *
 *  ファイルのコピー＆貼り付け(IIV_S_FX / IIV_C_FX。中身の形は filexfer.c)
 *    両方が IIV_HF_FXBATCH / IIV_WF_FXBATCH を出したときだけ、小さいファイルをまとめて読み
 *    (FX_READMANY)、中身を圧縮して返す(FX_DATAZ)。片方が古ければ、1 ファイルずつ圧縮せずに送る。
 * ================================================================== */
#ifndef IIVPROTO_H
#define IIVPROTO_H

#define IIV_MAGIC        0x31564949u     /* "IIV1" */
#define IIV_VERSION      1
#define IIV_DEFAULT_PORT 5960
#define IIV_MAX_INFLIGHT 2
#define IIV_MAX_MSG      (64 << 20)      /* 1 つのメッセージの上限 */
#define IIV_PBKDF2_ITER  100000

/* 認証の種類 */
#define IIV_AUTH_NONE     0
#define IIV_AUTH_PASSWORD 1

/* IivWelcome の結果 */
#define IIV_OK            0
#define IIV_BAD_PASSWORD  1
#define IIV_BLOCKED       2
#define IIV_BAD_VERSION   3

/* 映像の方式 */
#define IIV_CODEC_H264    1

#pragma pack(push, 1)
typedef struct IivHello {
    unsigned int   magic, version;
    unsigned int   codecs;              /* 1 << IIV_CODEC_H264 など */
    unsigned int   flags;               /* IIV_HF_* */
} IivHello;
#define IIV_HF_FILES      1u            /* ファイルのコピー＆貼り付けができる */
#define IIV_HF_FXBATCH    2u            /* ファイルのまとめ読み(FX_READMANY)と圧縮(FX_DATAZ)が分かる */

typedef struct IivChallenge {
    unsigned int   magic, version;
    unsigned int   auth;
    unsigned char  salt[16];            /* 操作できるパスワード */
    unsigned int   iterations;          /* 0 = このパスワードは無い */
    unsigned char  saltView[16];        /* 見るだけのパスワード */
    unsigned int   iterationsView;      /* 0 = このパスワードは無い */
    unsigned char  nonce[32];
} IivChallenge;

typedef struct IivWelcome {
    unsigned int   result;
    unsigned int   flags;               /* IIV_WF_* */
    unsigned short nameLen;             /* 続く名前(UTF-8)のバイト数 */
} IivWelcome;
#define IIV_WF_VIEWONLY   1u
#define IIV_WF_FILES      2u            /* サーバーもファイルを受け渡せる */
#define IIV_WF_FXBATCH    4u            /* サーバーもまとめ読みと圧縮が分かる(IIV_HF_FXBATCH と同じ) */
#pragma pack(pop)

/* ------------------------------------------------------------------ */
/*  サーバー → クライアント                                             */
/* ------------------------------------------------------------------ */

enum {
    IIV_S_VIDEO_CONFIG = 1, /* IivVideoConfig。次の IIV_S_VIDEO はキーフレーム */
    IIV_S_VIDEO,            /* IivVideoHead + Annex B */
    IIV_S_CURSOR_SHAPE,     /* IivCursorShape + BGRA(w*h*4) + 見える印((w+7)/8*h、1 = 見える) */
    IIV_S_CURSOR_POS,       /* IivCursorPos */
    IIV_S_CLIPBOARD,        /* UTF-8(CRLF) */
    IIV_S_FX,               /* u8 種類 + 中身(filexfer.c) */
    IIV_S_PING,             /* IivPing(往復の時間を測る) */
    IIV_S_AUDIO_CONFIG,     /* IivAudioConfig */
    IIV_S_AUDIO,            /* IivAudioHead + 中身(PCM: 16bit のサンプルを左右交互に。空 = 無音。AAC: raw の 1 フレーム) */
};

/* 音の方式 */
#define IIV_AUDIO_OFF     0
#define IIV_AUDIO_PCM     1             /* 音質優先: 16bit ステレオのまま(48kHz で 1.5Mbps)。遅れが増えない */
#define IIV_AUDIO_AAC     2             /* 速度優先: AAC-LC 96kbps(帯域は PCM の 1/16。符号化と復号で 70〜90ms 遅れる) */

/* IivAudioConfig の状態 */
#define IIV_AS_OK         0
#define IIV_AS_DISABLED   1             /* サーバーの「音を鳴らす」が切られている */
#define IIV_AS_NODEVICE   2             /* サーバーで音を取り込めない(再生デバイスが無いなど) */

#pragma pack(push, 1)
typedef struct IivVideoConfig {
    unsigned int   codec;
    unsigned short videoW, videoH;      /* 符号化した絵の大きさ */
    int            deskX, deskY;        /* 取り込んだ範囲(仮想画面の座標。参考) */
    unsigned short deskW, deskH;        /* マウスの座標はこの大きさの範囲で送る */
    long long      qpcFreq;             /* サーバーの QueryPerformanceFrequency(遅れの計測用) */
} IivVideoConfig;

typedef struct IivVideoHead {
    unsigned int   frame;               /* 1 から増える番号 */
    unsigned char  flags;               /* IIV_VF_* */
    long long      presentQpc;          /* サーバーの画面に出た時刻(DXGI の LastPresentTime。無ければ取り込んだ時刻) */
    long long      sendQpc;             /* 送り出した時刻 */
} IivVideoHead;
#define IIV_VF_KEY        1u

typedef struct IivCursorShape {
    unsigned short w, h, hotX, hotY;
    unsigned char  visible;
} IivCursorShape;

typedef struct IivCursorPos {
    int            x, y;                /* 取り込んだ範囲の中の画素の位置 */
    unsigned char  visible;
} IivCursorPos;

typedef struct IivPing {
    long long      qpc;
} IivPing;

typedef struct IivAudioConfig {
    unsigned char  codec;               /* IIV_AUDIO_*(OFF = 送らない) */
    unsigned char  status;              /* IIV_AS_* */
    unsigned char  channels;            /* 2 */
    unsigned int   rate;                /* 44100 か 48000 */
    unsigned short asc;                 /* AAC: AudioSpecificConfig(上位バイトから。例 0x1190 = LC・48kHz・2ch) */
} IivAudioConfig;

typedef struct IivAudioHead {
    unsigned int   seq;                 /* 1 から増える番号(抜けたら捨てられた) */
    unsigned int   frames;              /* この中身のサンプル数(1 チャンネルあたり) */
    long long      captureQpc;          /* その音を取り込んだ時刻(サーバーの QPC。遅れの計測用) */
} IivAudioHead;
#pragma pack(pop)

/* ------------------------------------------------------------------ */
/*  クライアント → サーバー                                             */
/* ------------------------------------------------------------------ */

enum {
    IIV_C_ACK = 1,          /* IivAck: 表示したフレーム */
    IIV_C_KEY,              /* IivKey */
    IIV_C_MOUSE,            /* IivMouse */
    IIV_C_CLIPBOARD,        /* UTF-8(CRLF) */
    IIV_C_FX,               /* u8 種類 + 中身(filexfer.c) */
    IIV_C_KEYFRAME,         /* 中身なし: キーフレームを求める(絵が壊れたとき) */
    IIV_C_SAS,              /* 中身なし: Ctrl+Alt+Del */
    IIV_C_PONG,             /* IivPing を送り返す */
    IIV_C_SETTINGS,         /* IivSettings */
    IIV_C_AUDIO,            /* IivAudioRequest */
};

#pragma pack(push, 1)
typedef struct IivAck {
    unsigned int   frame;
    unsigned int   decodeUs;            /* 受け取ってから表示までにかかった時間(マイクロ秒。統計用) */
} IivAck;

typedef struct IivKey {
    unsigned short scan;                /* スキャン コード。0x100 = 拡張(E0) */
    unsigned short vk;                  /* 仮想キー(スキャン コードで送りにくいキーのため) */
    unsigned char  down;
} IivKey;

typedef struct IivMouse {
    int            x, y;                /* 取り込んだ範囲(deskW x deskH)の中の画素の位置 */
    unsigned char  buttons;             /* IIV_MB_* */
    short          wheel, hwheel;       /* WHEEL_DELTA 単位の量(押した瞬間だけ) */
} IivMouse;
#define IIV_MB_LEFT    1u
#define IIV_MB_RIGHT   2u
#define IIV_MB_MIDDLE  4u
#define IIV_MB_X1      8u
#define IIV_MB_X2      16u

typedef struct IivSettings {
    unsigned int   kbps;                /* 0 = サーバーに任せる */
} IivSettings;

typedef struct IivAudioRequest {
    unsigned char  codec;               /* IIV_AUDIO_*(OFF = 要らない) */
} IivAudioRequest;
#pragma pack(pop)

#endif
