Texture2D gDiffuseMap : register(t0);
SamplerState gSampler : register(s0);

cbuffer ObjectTransformConstants : register(b0)
{
    float4x4 gWorld;
    float4x4 gWorldInvTranspose;
};

cbuffer GeometryFrameConstants : register(b1)
{
    float4x4 gView;
    float4x4 gProj;
};

cbuffer MaterialConstants : register(b2)
{
    float4 gMaterialDiffuse;
    float4 gMaterialSpecular;
    float gSpecularPower;
    int gHasTexture;
    float2 gPad;
};

struct VSInput
{
    float3 Position : POSITION;
    float3 Normal   : NORMAL;
    float2 TexCoord : TEXCOORD;
};

struct VSOutput
{
    float4 PositionH : SV_POSITION;
    float3 PositionW : TEXCOORD0;
    float3 NormalW   : TEXCOORD1;
    float2 TexCoord  : TEXCOORD2;
};

struct PSOutput
{
    float4 Albedo   : SV_Target0;
    float4 Normal   : SV_Target1;
    float4 Material : SV_Target2;
};

VSOutput VSMain(VSInput vin)
{
    VSOutput vout;
    float4 posW = mul(float4(vin.Position, 1.0f), gWorld);
    vout.PositionW = posW.xyz;
    vout.PositionH = mul(mul(posW, gView), gProj);
    vout.NormalW = normalize(mul(vin.Normal, (float3x3)gWorldInvTranspose));
    vout.TexCoord = vin.TexCoord;
    return vout;
}

PSOutput PSMain(VSOutput pin)
{
    PSOutput o;
    float4 albedo = gHasTexture ? gDiffuseMap.Sample(gSampler, pin.TexCoord) : gMaterialDiffuse;
    float3 n = normalize(pin.NormalW);

    o.Albedo = albedo;
    o.Normal = float4(n * 0.5f + 0.5f, 1.0f);
    o.Material = float4(gMaterialSpecular.rgb, saturate(gSpecularPower / 255.0f));
    return o;
}
