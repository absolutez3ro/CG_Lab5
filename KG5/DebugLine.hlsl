cbuffer DebugLineConstants : register(b0)
{
    float4x4 gViewProj;
};

struct VSInput
{
    float3 Pos : POSITION;
    float4 Color : COLOR;
};

struct PSInput
{
    float4 Pos : SV_POSITION;
    float4 Color : COLOR;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.Pos = mul(float4(input.Pos, 1.0f), gViewProj);
    output.Color = input.Color;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    return input.Color;
}
