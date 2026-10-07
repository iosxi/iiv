/* aac.h - AAC の符号化(サーバー)と復号(クライアント)。common/aac.c */
#ifndef IIV_AAC_H
#define IIV_AAC_H

typedef struct AacEnc AacEnc;
typedef struct AacDec AacDec;
typedef void (*AacOut)(void *ctx, const BYTE *p, int n);

/* 16bit ステレオ。rate は 44100 か 48000。呼ぶスレッドで COM を初期化しておく */
AacEnc *aacenc_open(int rate, int bytesPerSec);
unsigned short aacenc_asc(const AacEnc *e);                     /* AudioSpecificConfig(上位バイトから) */
BOOL    aacenc_put(AacEnc *e, const short *pcm, int frames, AacOut out, void *ctx);   /* 出たフレームを 1 つずつ out へ */
void    aacenc_close(AacEnc *e);

AacDec *aacdec_open(int rate, int channels, unsigned short asc);
int     aacdec_decode(AacDec *d, const BYTE *p, int n, const short **pcm);  /* 出たサンプル数(1 チャンネルあたり)。-1 = 失敗 */
void    aacdec_close(AacDec *d);

#endif
