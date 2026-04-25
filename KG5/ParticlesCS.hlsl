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

RWStructuredBuffer<Particle> g_Particles : register(u0);
ConsumeStructuredBuffer<uint> g_DeadListConsume : register(u1);
AppendStructuredBuffer<uint> g_DeadListAppend : register(u1);
AppendStructuredBuffer<SortEntry> g_AliveSortList : register(u2);
RWStructuredBuffer<SortEntry> g_SortList : register(u2);

cbuffer Cb : register(b0)
{
    float4 C0;
    float4 C1;
    float4 C2;
    float4 C3;
    float4 C4;
    float4 C5;
};

float HashFloat(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352d;
    x ^= x >> 15;
    x *= 0x846ca68b;
    x ^= x >> 16;
    return (x & 0x00FFFFFFu) / 16777215.0;
}

[numthreads(256,1,1)]
void InitDeadListCS(uint3 DTid : SV_DispatchThreadID)
{
    uint index = DTid.x;
    uint maxParticles = asuint(C0.z);
    if (index >= maxParticles) return;

    Particle p = (Particle)0;
    p.Life = -1.0;
    p.LifeSpan = 0.0;
    p.Age = 0.0;
    g_Particles[index] = p;

    SortEntry s;
    s.ParticleIndex = 0xFFFFFFFFu;
    s.DistanceSq = -1.0;
    s.Padding0 = 0.0;
    s.Padding1 = 0.0;
    g_SortList[index] = s;

    g_DeadListAppend.Append(index);
}

[numthreads(256,1,1)]
void EmitCS(uint3 DTid : SV_DispatchThreadID)
{
    uint i = DTid.x;

    float3 emitter = C0.xyz;
    uint emitCount = asuint(C0.w);
    float3 baseVel = C1.xyz;
    float time = C1.w;
    float3 velRnd = C2.xyz;
    float minLife = C2.w;
    float maxLife = C3.x;
    float minSize = C3.y;
    float maxSize = C3.z;
    uint seed = asuint(C3.w);
    float4 colorA = C4;
    float4 colorB = C5;

    if (i >= emitCount) return;

    uint deadIndex = g_DeadListConsume.Consume();
    uint h0 = i * 1664525u + seed + asuint(time * 1000.0);
    uint h1 = h0 * 22695477u + 1u;
    uint h2 = h1 * 22695477u + 1u;

    float rx = HashFloat(h0) * 2.0 - 1.0;
    float ry = HashFloat(h1) * 2.0 - 1.0;
    float rz = HashFloat(h2) * 2.0 - 1.0;
    float t = HashFloat(h2 ^ 0x9e3779b9u);

    Particle p;
    p.Position = emitter + float3(rx, ry, rz) * 2.5;
    p.Velocity = baseVel + float3(rx * velRnd.x, abs(ry) * velRnd.y, rz * velRnd.z);
    p.LifeSpan = lerp(minLife, maxLife, t);
    p.Life = p.LifeSpan;
    p.Color = lerp(colorA, colorB, t);
    p.Size = lerp(minSize, maxSize, HashFloat(h1 ^ 0x85ebca6bu));
    p.Weight = 0.7 + HashFloat(h2 ^ 0xc2b2ae35u) * 0.8;
    p.Age = 0.0;
    p.Padding = 0.0;

    g_Particles[deadIndex] = p;
}

[numthreads(256,1,1)]
void UpdateCS(uint3 DTid : SV_DispatchThreadID)
{
    uint index = DTid.x;

    float dt = C0.x;
    uint maxParticles = asuint(C0.z);
    float3 gravity = C1.xyz;
    float groundY = C1.w;
    float3 cameraPos = C2.xyz;
    uint useGround = asuint(C2.w);

    if (index >= maxParticles) return;

    Particle p = g_Particles[index];
    if (p.Life <= 0.0) return;

    p.Age += dt;
    p.Life -= dt;
    p.Velocity += gravity * p.Weight * dt;
    p.Position += p.Velocity * dt;

    if (p.Life <= 0.0 || (useGround != 0 && p.Position.y <= groundY))
    {
        p.Life = -1.0;
        g_Particles[index] = p;
        g_DeadListAppend.Append(index);
        return;
    }

    g_Particles[index] = p;
    float3 toCam = p.Position - cameraPos;

    SortEntry s;
    s.ParticleIndex = index;
    s.DistanceSq = dot(toCam, toCam);
    s.Padding0 = 0.0;
    s.Padding1 = 0.0;
    g_AliveSortList.Append(s);
}

[numthreads(256,1,1)]
void BitonicSortCS(uint3 DTid : SV_DispatchThreadID)
{
    uint i = DTid.x;
    uint elementCount = asuint(C0.x);
    uint subArray = asuint(C0.y);
    uint compareDistance = asuint(C0.z);
    uint sortDescending = asuint(C0.w);

    if (i >= elementCount) return;

    uint j = i ^ compareDistance;
    if (j > i && j < elementCount)
    {
        SortEntry a = g_SortList[i];
        SortEntry b = g_SortList[j];

        bool ascending = ((i & subArray) == 0);
        if (sortDescending != 0)
            ascending = !ascending;

        bool swapNeeded = ascending ? (a.DistanceSq > b.DistanceSq) : (a.DistanceSq < b.DistanceSq);
        if (swapNeeded)
        {
            g_SortList[i] = b;
            g_SortList[j] = a;
        }
    }
}
