Texture2D gDiffuseMap      : register(t0);
Texture2D gNormalMap       : register(t1);
Texture2D gDisplacementMap : register(t2);
SamplerState gSampler : register(s0);

cbuffer ObjectTransformConstants : register(b0)
{
    float4x4 gWorld;
    float4x4 gWorldInvTranspose;
    float4 gColorTint;
};

cbuffer GeometryFrameConstants : register(b1)
{
    float4x4 gView;
    float4x4 gProj;
    float4 gCameraPos;
    float2 gTessFactorRange;
    float2 gTessDistanceRange;
    uint gGeometryDebugMode;
    uint gDebugStrongDisplacement;
    float2 gGeometryFramePad;
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

struct VSInput
{
    float3 Position  : POSITION;
    float3 Normal    : NORMAL;
    float2 TexCoord  : TEXCOORD;
    float3 Tangent   : TANGENT;
    float3 Bitangent : BITANGENT;
};

struct VSOutput
{
    float3 PositionW  : POSITION;
    float3 NormalW    : NORMAL;
    float2 TexCoord   : TEXCOORD;
    float3 TangentW   : TANGENT;
    float3 BitangentW : BITANGENT;
    float4 ColorTint  : COLOR0;
};

struct HSConstantsData
{
    float EdgeTess[3] : SV_TessFactor;
    float InsideTess  : SV_InsideTessFactor;
};

struct DSOutput
{
    float4 PositionH  : SV_POSITION;
    float3 PositionW  : TEXCOORD0;
    float3 NormalW    : TEXCOORD1;
    float2 TexCoord   : TEXCOORD2;
    float3 TangentW   : TEXCOORD3;
    float3 BitangentW : TEXCOORD4;
    float4 ColorTint  : COLOR0;
    float TessFactorN : TEXCOORD5;
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
    vout.NormalW = normalize(mul(vin.Normal, (float3x3)gWorldInvTranspose));
    vout.TexCoord = vin.TexCoord;
    vout.TangentW = normalize(mul(vin.Tangent, (float3x3)gWorld));
    vout.BitangentW = normalize(mul(vin.Bitangent, (float3x3)gWorld));
    vout.ColorTint = gColorTint;
    return vout;
}

HSConstantsData HSConstants(
    InputPatch<VSOutput, 3> patch,
    uint patchID : SV_PrimitiveID)
{
    HSConstantsData o;

    const float kSafetyMaxTess = 16.0f;

    const float3 p0 = patch[0].PositionW;
    const float3 p1 = patch[1].PositionW;
    const float3 p2 = patch[2].PositionW;
    const float3 patchCenter = (p0 + p1 + p2) / 3.0f;
    const float distanceToCamera = distance(patchCenter, gCameraPos.xyz);

    const float minDist = min(gTessDistanceRange.x, gTessDistanceRange.y);
    const float maxDist = max(gTessDistanceRange.x, gTessDistanceRange.y);
    const float distRange = max(1e-4f, maxDist - minDist);
    const float distNorm = saturate((distanceToCamera - minDist) / distRange);
    const float distLerp = smoothstep(0.0f, 1.0f, distNorm);

    const float minTess = clamp(min(gTessFactorRange.x, gTessFactorRange.y), 1.0f, kSafetyMaxTess);
    const float maxTess = clamp(max(gTessFactorRange.x, gTessFactorRange.y), minTess, kSafetyMaxTess);

    float tess = lerp(maxTess, minTess, distLerp);

    tess = clamp(tess, minTess, maxTess);

    o.EdgeTess[0] = tess;
    o.EdgeTess[1] = tess;
    o.EdgeTess[2] = tess;
    o.InsideTess = tess;
    return o;
}

[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("HSConstants")]
VSOutput HSMain(
    InputPatch<VSOutput, 3> patch,
    uint i : SV_OutputControlPointID,
    uint patchID : SV_PrimitiveID)
{
    return patch[i];
}

[domain("tri")]
DSOutput DSMain(
    HSConstantsData hsConst,
    float3 bary : SV_DomainLocation,
    const OutputPatch<VSOutput, 3> patch)
{
    DSOutput o;

    float3 basePositionW =
        patch[0].PositionW * bary.x +
        patch[1].PositionW * bary.y +
        patch[2].PositionW * bary.z;

    float3 displacedPositionW = basePositionW;

    float3 normalW = normalize(
        patch[0].NormalW * bary.x +
        patch[1].NormalW * bary.y +
        patch[2].NormalW * bary.z);

    float2 texCoord =
        patch[0].TexCoord * bary.x +
        patch[1].TexCoord * bary.y +
        patch[2].TexCoord * bary.z;

    float3 tangentW = normalize(
        patch[0].TangentW * bary.x +
        patch[1].TangentW * bary.y +
        patch[2].TangentW * bary.z);

    float3 bitangentW = normalize(
        patch[0].BitangentW * bary.x +
        patch[1].BitangentW * bary.y +
        patch[2].BitangentW * bary.z);

    if (gHasDisplacementMap != 0)
    {
        float displacementTex = gDisplacementMap.SampleLevel(gSampler, texCoord, 0).r;
        float centeredDisplacement = displacementTex * 2.0f - 1.0f;
        float displacement = centeredDisplacement * gDisplacementScale + gDisplacementBias;
        if (gDebugStrongDisplacement != 0)
        {
            displacement *= 3.0f;
        }
        displacedPositionW += normalW * displacement;
    }

    o.PositionW = displacedPositionW;
    o.PositionH = mul(mul(float4(displacedPositionW, 1.0f), gView), gProj);
    o.NormalW = normalW;
    o.TangentW = tangentW;
    o.BitangentW = bitangentW;
    o.TexCoord = texCoord;
    o.TessFactorN = saturate(hsConst.InsideTess / 16.0f);
    o.ColorTint = gColorTint;
    return o;
}

PSOutput PSMain(DSOutput pin)
{
    PSOutput o;
    float4 albedo = gHasTexture ? gDiffuseMap.Sample(gSampler, pin.TexCoord) : gMaterialDiffuse;
    albedo.rgb *= pin.ColorTint.rgb;
    float3 n = normalize(pin.NormalW);

    if (gHasNormalMap != 0)
    {
        float3 t = normalize(pin.TangentW);
        t = normalize(t - n * dot(t, n));

        float3 bRef = normalize(pin.BitangentW);
        float handedness = (dot(cross(n, t), bRef) < 0.0f) ? -1.0f : 1.0f;
        float3 b = normalize(cross(n, t)) * handedness;

        float3 normalTS = gNormalMap.Sample(gSampler, pin.TexCoord).xyz;
        normalTS = normalize(normalTS * 2.0f - 1.0f);

        float3x3 tbn = float3x3(t, b, n);
        n = normalize(mul(normalTS, tbn));
    }

    if (gGeometryDebugMode == 1)
    {
        float3 debugNormal = n * 0.5f + 0.5f;
        o.Albedo = float4(debugNormal, 1.0f);
        o.Normal = float4(0.5f, 0.5f, 1.0f, 1.0f);
        o.Material = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return o;
    }

    if (gGeometryDebugMode == 2)
    {
        float displacementGray = 0.0f;
        if (gHasDisplacementMap != 0)
        {
            float displacementTex = gDisplacementMap.SampleLevel(gSampler, pin.TexCoord, 0).r;
            displacementGray = saturate(displacementTex);
        }

        o.Albedo = float4(displacementGray, displacementGray, displacementGray, 1.0f);
        o.Normal = float4(0.5f, 0.5f, 1.0f, 1.0f);
        o.Material = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return o;
    }

    if (gGeometryDebugMode == 3)
    {
        float tessN = saturate(pin.TessFactorN);
        float3 tessColor = lerp(float3(0.0f, 0.0f, 1.0f), float3(1.0f, 0.15f, 0.0f), tessN);
        o.Albedo = float4(tessColor, 1.0f);
        o.Normal = float4(0.5f, 0.5f, 1.0f, 1.0f);
        o.Material = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return o;
    }

    o.Albedo = albedo;
    o.Normal = float4(n * 0.5f + 0.5f, 1.0f);
    o.Material = float4(gMaterialSpecular.rgb, saturate(gSpecularPower / 255.0f));
    return o;
}

struct VSNoTessOutput
{
    float4 PositionH  : SV_POSITION;
    float3 PositionW  : TEXCOORD0;
    float3 NormalW    : TEXCOORD1;
    float2 TexCoord   : TEXCOORD2;
    float3 TangentW   : TEXCOORD3;
    float3 BitangentW : TEXCOORD4;
};

VSNoTessOutput VSMainNoTess(VSInput vin)
{
    VSNoTessOutput o;
    float4 posW = mul(float4(vin.Position, 1.0f), gWorld);
    o.PositionW = posW.xyz;
    o.PositionH = mul(mul(posW, gView), gProj);
    o.NormalW = normalize(mul(vin.Normal, (float3x3)gWorldInvTranspose));
    o.TexCoord = vin.TexCoord;
    o.TangentW = normalize(mul(vin.Tangent, (float3x3)gWorld));
    o.BitangentW = normalize(mul(vin.Bitangent, (float3x3)gWorld));
    return o;
}

PSOutput PSMainNoTess(VSNoTessOutput pin)
{
    DSOutput ds;
    ds.PositionH = pin.PositionH;
    ds.PositionW = pin.PositionW;
    ds.NormalW = pin.NormalW;
    ds.TexCoord = pin.TexCoord;
    ds.TangentW = pin.TangentW;
    ds.BitangentW = pin.BitangentW;
    ds.ColorTint = gColorTint;
    ds.TessFactorN = 0.0f;
    return PSMain(ds);
}
