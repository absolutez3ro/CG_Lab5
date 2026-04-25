struct Particle
{
    float3 Position;
    float Life;
    float3 Velocity;
    float LifeSpan;
    float4 Color;
    float Size;
    float Weight;
    float Age;
    float Rotation;
};

struct SortEntry
{
    uint ParticleIndex;
    float DistanceSq;
    float Padding0;
    float Padding1;
};

StructuredBuffer<Particle> g_Particles : register(t0);
StructuredBuffer<SortEntry> g_SortList : register(t1);
Texture2D gSmokeTexture : register(t2);
SamplerState gSmokeSampler : register(s0);

cbuffer ParticleRenderConstants : register(b0)
{
    float4x4 gView;
    float4x4 gProj;
    float4x4 gViewProj;
    float3 gCameraRight;
    float gPadding0;
    float3 gCameraUp;
    float gPadding1;
    float3 gDirectionalLightDir;
    float gDirectionalLightIntensity;
    float4 gDirectionalLightColor;
    float4 gAmbientColor;
};

struct VSInput
{
    float2 Corner : POSITION;
    uint InstanceID : SV_InstanceID;
};

struct VSOutput
{
    float4 PositionH : SV_POSITION;
    float4 Color : COLOR;
    float2 UV : TEXCOORD0;
    float Life01 : TEXCOORD1;
};

VSOutput VSMain(VSInput input)
{
    VSOutput o;
    SortEntry se = g_SortList[input.InstanceID];
    if (se.ParticleIndex == 0xFFFFFFFFu)
    {
        o.PositionH = float4(-9999.0, -9999.0, -9999.0, 1.0);
        o.Color = float4(0,0,0,1);
        o.UV = float2(0.0, 0.0);
        o.Life01 = 0.0;
        return o;
    }

    Particle p = g_Particles[se.ParticleIndex];
    float s = sin(p.Rotation);
    float c = cos(p.Rotation);
    float2 rc = float2(
        input.Corner.x * c - input.Corner.y * s,
        input.Corner.x * s + input.Corner.y * c
    );
    float life01 = saturate(p.Life / max(p.LifeSpan, 0.001));
    float age01 = 1.0 - life01;
    float sizeGrow = lerp(0.65, 1.6, age01);
    float particleSize = p.Size * sizeGrow;
    float3 worldPos = p.Position + gCameraRight * rc.x * particleSize + gCameraUp * rc.y * particleSize;

    float3 viewDir = normalize(-worldPos);
    float3 normal = normalize(float3(0.0, 1.0, 0.0) + viewDir * 0.2);
    float3 L = normalize(-gDirectionalLightDir);
    float ndl = max(dot(normal, L), 0.0);
    float3 light = gAmbientColor.rgb + gDirectionalLightColor.rgb * (gDirectionalLightIntensity * ndl);

    o.Color = float4(saturate(p.Color.rgb * light), p.Color.a);
    o.UV = input.Corner + 0.5;
    o.Life01 = life01;
    o.PositionH = mul(float4(worldPos, 1.0), gViewProj);
    return o;
}

float4 PSMain(VSOutput input) : SV_Target
{
    float4 tex = gSmokeTexture.Sample(gSmokeSampler, input.UV);
    if (tex.a < 0.03f)
    {
        discard;
    }

    float texAlpha = smoothstep(0.05f, 0.75f, tex.a);
    float life01 = saturate(input.Life01);
    float fadeIn = smoothstep(0.0f, 0.25f, life01);
    float fadeOut = smoothstep(0.0f, 0.45f, 1.0f - life01);
    float lifeFade = fadeIn * fadeOut;

    float alpha = input.Color.a * texAlpha * lifeFade;
    alpha = min(alpha, 0.18f);

    if (alpha < 0.01f)
    {
        discard;
    }

    float3 smokeColor = input.Color.rgb * lerp(0.75f, 1.1f, texAlpha);
    return float4(smokeColor, alpha);
}
