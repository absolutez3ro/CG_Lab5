Texture2D gDiffuseMap : register(t0);
SamplerState gSampler : register(s0);

cbuffer ObjectCB : register(b0)
{
    float4x4 gWorld;
    float4x4 gWorldInvTranspose;
    float4 gTint;
};

cbuffer ShadowFrameCB : register(b1)
{
    float4x4 gShadowViewProj;
};

cbuffer MaterialConstants : register(b2)
{
    float4 gMaterialDiffuse;
    float4 gMaterialSpecular;
    float gSpecularPower;
    int gHasTexture;
    int gHasNormalMap;
    int gHasDisplacementMap;
    float gDisplacementScale;
    float gDisplacementBias;
    float2 gMaterialPad;
};

struct VSIn
{
    float3 pos : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD;
    float3 tangent : TANGENT;
    float3 bitangent : BITANGENT;
};

struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VSOut VSMain(VSIn i)
{
    VSOut o;
    float4 worldPos = mul(float4(i.pos, 1.0f), gWorld);
    o.pos = mul(worldPos, gShadowViewProj);
    o.uv = i.uv;
    return o;
}

void PSMain(VSOut pin)
{
    const float textureAlpha = (gHasTexture != 0) ? gDiffuseMap.Sample(gSampler, pin.uv).a : 1.0f;
    const float finalAlpha = textureAlpha * gMaterialDiffuse.a;
    clip(finalAlpha - 0.5f);
}
