// view.hlsl - 相手の画面を窓に描く(build.bat が fxc でバイト列にして埋め込む)
//
// 頂点は持たない。SV_VertexID から画面全体を覆う三角形を 1 つ作り、
// ビューポート(= 絵を置く矩形)いっぱいに貼る。縮めるときはミップマップを
// 使う(4K を半分にしても文字がちらつかない)。

Texture2D    tex : register(t0);
SamplerState smp : register(s0);

struct VO {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VO vs(uint id : SV_VertexID)
{
    VO o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv  = uv;
    return o;
}

float4 ps(VO i) : SV_Target
{
    return float4(tex.Sample(smp, i.uv).rgb, 1);
}

// NV12(BT.709、16〜235)→ RGB。復号した絵(GPU)を、表示用のテクスチャ(BGRA)へ写すときに使う。
// 画素の位置で Y と UV を直接読む(ビューポート = 映像の大きさ。復号した絵は 16 の倍数に
// 切り上げられていることがあるが、はみ出した所は読まない)。
Texture2D<float>  texY  : register(t0);
Texture2D<float2> texUV : register(t1);

float4 ps_nv12(VO i) : SV_Target
{
    uint2  p  = uint2(i.pos.xy);
    float  y  = (texY.Load(int3(p, 0)) - 16.0 / 255.0) * 1.164384;
    float2 c  = texUV.Load(int3(p >> 1, 0)) - 128.0 / 255.0;
    float  r  = y + 1.792741 * c.y;
    float  g  = y - 0.213249 * c.x - 0.532909 * c.y;
    float  b  = y + 2.112402 * c.x;
    return float4(saturate(float3(r, g, b)), 1);
}
