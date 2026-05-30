cbuffer PostProcessCB : register(b0)
{
    float2 gScreenSize;
    float2 gInvScreenSize;
    float gTime;
    uint gMode;
    float gEdgeStrength;
    float gDepthEdgeScale;
    float gNormalEdgeScale;
    float gLumaEdgeScale;
    float gVcrIntensity;
    float gScanlineStrength;
    float gNoiseStrength;
    float gChromaticAberration;
    float gScannerIntensity;
    float gScannerSpeed;
    float gScannerLineWidth;
    float gScannerTickStrength;
    float gNauseaIntensity;
    float gNauseaSpeed;
    float gKaleidoscopeSegments;
    float gNauseaChromaticAberration;
    float2 gPadding0;
    float4x4 gInvViewProj;
    float4 gEyePos;
    float gScannerMaxDistance;
    float gScannerWorldLineWidth;
    float gScannerTrailLength;
    float gScannerGridScale;
    float gExposure;
    float gGamma;
    uint gToneMapperMode;
    float gToneMapWhitePoint;
};

Texture2D<float4> gSceneColor : register(t0);
Texture2D<float4> gAlbedoTex : register(t1);
Texture2D<float4> gNormalTex : register(t2);
Texture2D<float4> gMaterialTex : register(t3);
Texture2D<float> gDepthTex : register(t4);
SamplerState gPointClamp : register(s0);
SamplerState gLinearClamp : register(s1);

struct VSOut { float4 Pos : SV_Position; float2 Uv : TEXCOORD0; };
VSOut VSMain(uint vid : SV_VertexID)
{
    float2 p[6] = { {-1,-1},{-1,1},{1,1},{-1,-1},{1,1},{1,-1} };
    VSOut o;
    o.Pos = float4(p[vid], 0, 1);
    o.Uv = float2((p[vid].x + 1) * 0.5, 1 - (p[vid].y + 1) * 0.5);
    return o;
}

float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
float Hash(float2 p) { return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }
bool HasValidSurface(float depth) { return depth < 0.999999f; }
float3 ReconstructWorldPosition(float2 uv, float depth)
{
    float2 ndc = float2(uv.x * 2.0f - 1.0f, (1.0f - uv.y) * 2.0f - 1.0f);
    float4 clipPos = float4(ndc, depth, 1.0f);
    float4 worldPos = mul(clipPos, gInvViewProj);
    return worldPos.xyz / max(worldPos.w, 1e-6f);
}

float ComputeGeometryEdge(float2 uv)
{
    float gx[9] = { -1,0,1, -2,0,2, -1,0,1 };
    float gy[9] = { -1,-2,-1, 0,0,0, 1,2,1 };
    float2 o[9] = {
        float2(-1,-1), float2(0,-1), float2(1,-1),
        float2(-1, 0), float2(0, 0), float2(1, 0),
        float2(-1, 1), float2(0, 1), float2(1, 1)
    };

    float dx = 0, dy = 0;
    float3 ngx = 0, ngy = 0;
    [unroll] for (int i = 0; i < 9; ++i)
    {
        float2 suv = uv + o[i] * gInvScreenSize;
        float d = gDepthTex.Sample(gPointClamp, suv);
        float3 n = normalize(gNormalTex.Sample(gPointClamp, suv).xyz * 2.0 - 1.0);
        dx += d * gx[i];
        dy += d * gy[i];
        ngx += n * gx[i];
        ngy += n * gy[i];
    }

    float depthEdge = length(float2(dx, dy));
    float normalEdge = length(ngx) + length(ngy);
    float edgeRaw = depthEdge * gDepthEdgeScale + normalEdge * gNormalEdgeScale;
    float edge = smoothstep(0.25, 0.70, edgeRaw);
    return pow(edge, 0.75);
}

float3 ApplyScanner(float2 uv, float3 scene)
{
    float depth = gDepthTex.Sample(gPointClamp, uv).r;
    if (!HasValidSurface(depth)) return scene;

    float3 worldPos = ReconstructWorldPosition(uv, depth);
    float distToCamera = distance(worldPos, gEyePos.xyz);
    float scan01 = frac(gTime * gScannerSpeed);
    float scanRadius = scan01 * gScannerMaxDistance;
    float waveDist = abs(distToCamera - scanRadius);
    float wave = 1.0 - smoothstep(0.0, gScannerWorldLineWidth, waveDist);

    float behind = scanRadius - distToCamera;
    float trail = saturate(behind / max(gScannerTrailLength, 1.0));
    trail *= step(0.0, behind);
    trail *= 0.18;

    float edge = ComputeGeometryEdge(uv) * gEdgeStrength;
    float activeMask = saturate(wave + trail);
    float contour = 1.0 - abs(frac(distToCamera / max(gScannerGridScale, 1.0) - gTime * 0.35) * 2.0 - 1.0);
    contour = pow(saturate(contour), 18.0);
    contour *= activeMask;

    float tickPattern = step(0.88, frac(worldPos.x * 0.08 + worldPos.z * 0.08 + floor(worldPos.y * 0.025) * 0.37 + gTime * 2.0));
    tickPattern *= wave;

    float3 cyan = float3(0.0, 0.85, 1.0);
    float3 deepCyan = float3(0.0, 0.35, 0.55);
    float3 result = scene;

    result = lerp(result, scene * float3(0.75, 0.9, 1.0), activeMask * 0.18);
    result += cyan * wave * 0.35 * gScannerIntensity;
    result += cyan * edge * activeMask * 1.8 * gScannerIntensity;
    result += deepCyan * contour * 0.8 * gScannerIntensity;
    result += cyan * contour * wave * 0.9 * gScannerIntensity;
    result += cyan * tickPattern * gScannerTickStrength * 0.35;
    return saturate(result);
}

float3 ApplyVCR(float2 uv, float3 inputColor)
{
    float v = gVcrIntensity;
    float scan = sin((uv.y * gScreenSize.y + gTime * 90.0) * 1.2) * gScanlineStrength * v;
    float n = (Hash(uv * gScreenSize + gTime * 31) - 0.5) * gNoiseStrength * v;
    float jitter = (Hash(float2(floor(uv.y * gScreenSize.y * 0.5), gTime * 5)) - 0.5) * 0.005 * v;
    float barY = frac(gTime * 0.17);
    float bar = 1.0 - smoothstep(0.0, 0.035, abs(uv.y - barY));
    float tear = bar * 0.04 * v;

    float2 uvj = uv + float2(jitter + tear, 0);
    float ca = gChromaticAberration * 0.0015 * v;
    float r = gSceneColor.Sample(gLinearClamp, uvj + float2(ca, 0)).r;
    float g = gSceneColor.Sample(gLinearClamp, uvj).g;
    float b = gSceneColor.Sample(gLinearClamp, uvj - float2(ca, 0)).b;
    float3 caColor = float3(r, g, b);

    float3 col = lerp(inputColor, caColor, 0.15);
    col += scan + n;
    float vig = 1.0 - saturate(distance(uv, float2(0.5, 0.5)) * 1.2);
    col *= lerp(0.85, 1.05, vig);
    float gray = Luma(col);
    col = lerp(float3(gray, gray, gray), col, 0.82);
    col = pow(saturate(col), 1.05);
    return col;
}

float2 ApplyNauseaUV(float2 uv)
{
    float t = gTime * gNauseaSpeed;
    float2 d = uv;

    float2 wobble;
    wobble.x = sin(uv.y * 18.0 + t * 2.1) * 0.012 * gNauseaIntensity;
    wobble.y = cos(uv.x * 14.0 + t * 1.7) * 0.010 * gNauseaIntensity;
    d += wobble;

    float2 centered = d - 0.5;
    float swayAngle = sin(t * 1.3) * 0.035 * gNauseaIntensity;
    float sa = sin(swayAngle), ca = cos(swayAngle);
    centered = float2(centered.x * ca - centered.y * sa, centered.x * sa + centered.y * ca);
    d = centered + 0.5;

    float2 rc = d - 0.5;
    rc.x *= gScreenSize.x / gScreenSize.y;
    float r = length(rc);
    float2 dir = (r > 1e-4) ? normalize(rc) : float2(0.0, 0.0);
    d += dir * sin(r * 28.0 - t * 4.0) * 0.008 * gNauseaIntensity;

    float2 kc = d - 0.5;
    float ang = atan2(kc.y, kc.x);
    float rad = length(kc);
    float seg = max(2.0, gKaleidoscopeSegments);
    float sector = 6.2831853 / seg;
    float fold = abs(fmod(ang + sector * 0.5, sector) - sector * 0.5);
    float2 kuv = float2(cos(fold), sin(fold)) * rad + 0.5;

    float kBlend = saturate(0.25 * gNauseaIntensity);
    float2 outUv = lerp(d, kuv, kBlend);
    return saturate(outUv);
}

float3 ApplyNausea(float2 uv)
{
    float t = gTime * gNauseaSpeed;
    float2 duv = ApplyNauseaUV(uv);
    float2 fromCenter = duv - 0.5;
    float edge = saturate(length(fromCenter) * 2.2);
    float ca = gNauseaChromaticAberration * 0.0018 * (0.25 + edge);

    float3 col;
    col.r = gSceneColor.Sample(gLinearClamp, saturate(duv + float2(ca, 0))).r;
    col.g = gSceneColor.Sample(gLinearClamp, duv).g;
    col.b = gSceneColor.Sample(gLinearClamp, saturate(duv - float2(ca, 0))).b;

    float3 tint = float3(0.95, 1.03, 0.97);
    float pulse = 1.0 + sin(t * 2.5) * 0.06 * gNauseaIntensity;
    float mixWave = sin(t * 1.2 + uv.x * 4.0 + uv.y * 3.0) * 0.5 + 0.5;
    float3 altTint = lerp(float3(1.03, 0.95, 1.04), float3(0.94, 1.04, 0.97), mixWave);
    col *= lerp(tint, altTint, 0.35 * gNauseaIntensity);
    col *= pulse;
    return saturate(col);
}

float3 ToneMapReinhard(float3 hdr)
{
    return hdr / (hdr + 1.0);
}

float3 ToneMapExposure(float3 hdr)
{
    return 1.0 - exp(-hdr * gExposure);
}

float3 ToneMapACES(float3 x)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float3 ApplyToneMapping(float3 hdr)
{
    hdr = max(hdr, 0.0);

    if (gToneMapperMode == 0)
    {
        return saturate(hdr);
    }
    else if (gToneMapperMode == 1)
    {
        return ToneMapReinhard(hdr * gExposure);
    }
    else if (gToneMapperMode == 2)
    {
        return ToneMapExposure(hdr);
    }
    else
    {
        return ToneMapACES(hdr * gExposure);
    }
}

float3 ApplyGammaCorrection(float3 linearColor)
{
    linearColor = saturate(linearColor);
    float gamma = max(gGamma, 0.001);
    return pow(linearColor, 1.0 / gamma);
}

float3 FinalColorTransform(float3 hdr)
{
    float3 ldr = ApplyToneMapping(hdr);
    return ApplyGammaCorrection(ldr);
}

float4 PSMain(VSOut i) : SV_Target
{
    float2 uv = i.Uv;
    float3 scene = gSceneColor.Sample(gLinearClamp, uv).rgb;
    float3 c = scene;

    if (gMode == 0)
    {
        c = scene;
    }
    else if (gMode == 1)
    {
        c = ApplyScanner(uv, scene);
    }
    else if (gMode == 2)
    {
        c = ApplyVCR(uv, scene);
    }
    else if (gMode == 3)
    {
        c = ApplyScanner(uv, scene);
        c = ApplyVCR(uv, c);
    }
    else if (gMode == 4)
    {
        c = ApplyNausea(uv);
    }
    else if (gMode == 5)
    {
        c = ApplyNausea(uv);
        c = ApplyVCR(uv, c);
    }

    float3 finalColor = FinalColorTransform(c);
    return float4(finalColor, 1.0);
}
