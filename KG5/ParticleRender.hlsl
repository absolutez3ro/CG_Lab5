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
    float Padding;
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
};

VSOutput VSMain(VSInput input)
{
    VSOutput o;
    SortEntry se = g_SortList[input.InstanceID];
    if (se.ParticleIndex == 0xFFFFFFFFu)
    {
        o.PositionH = float4(-9999.0, -9999.0, -9999.0, 1.0);
        o.Color = float4(0,0,0,1);
        return o;
    }

    Particle p = g_Particles[se.ParticleIndex];
    float3 worldPos = p.Position + gCameraRight * input.Corner.x * p.Size + gCameraUp * input.Corner.y * p.Size;

    float3 viewDir = normalize(-worldPos);
    float3 normal = normalize(float3(0.0, 1.0, 0.0) + viewDir * 0.2);
    float3 L = normalize(-gDirectionalLightDir);
    float ndl = max(dot(normal, L), 0.0);
    float3 light = gAmbientColor.rgb + gDirectionalLightColor.rgb * (gDirectionalLightIntensity * ndl);

    o.Color = float4(saturate(p.Color.rgb * light), 1.0);
    o.PositionH = mul(float4(worldPos, 1.0), gViewProj);
    return o;
}

float4 PSMain(VSOutput input) : SV_Target
{
    return float4(input.Color.rgb, 1.0);
}
