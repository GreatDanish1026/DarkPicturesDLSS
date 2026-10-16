// Clamped copy of the DLSS output, for the game's screen-space lighting, plus brightness
// diagnostics.
//
// The Quarry's SSGI and SSR (enabled by the pak mods) light each frame from the
// previous frame's TAA output. Under UE's TAA that loop is bounded because TAA clamps
// its history to the current frame's colours; DLSS has no such clamp, so errors fed back
// through the lighting grow into echoes and blown-out highlights. This pass writes the
// DLSS output clamped to the min/max of the current frame's input colour around each
// pixel (3x3, in render pixels); dlss.cpp copies it over the texture the lighting reads
// next frame. The displayed image stays the unclamped DLSS output.
//
// Every 4th output pixel in each direction it also measures the luma of the input colour
// (the render pixel under it), the DLSS output and the clamped copy into Stats, which
// dlss.cpp logs every frame to show where a blow-up starts.

cbuffer Constants : register(b0)
{
    float2 OutSize;     // output view rect, pixels (origin 0,0)
    float2 RenderSize;  // render view rect, pixels (origin 0,0)
};

Texture2D<float4> InputColor : register(t0);   // the TAAU's input colour (render res)
RWTexture2D<float4> DlssOutput : register(u0);  // the TAAU's output texture, just written by DLSS
RWTexture2D<float4> Clamped : register(u1);     // ours, same size as the output texture

// Brightness counters, zeroed each frame. Sums are luma clipped to kSumClip, in 1/kSumScale
// units, so ~400k samples at 3840x1600 can't overflow 32 bits. Maxima are float bits
// (non-negative floats order like uints).
//  0 samples              4 max input luma        8 non-finite input samples
//  1 sum input luma       5 max DLSS luma         9 non-finite DLSS samples
//  2 sum DLSS luma        6 max clamped luma     10 DLSS luma > kHot
//  3 sum clamped luma     7 clamp cut > 20%      11 input luma > kHot
RWBuffer<uint> Stats : register(u2);

static const float kSumClip = 64.0;
static const float kSumScale = 64.0;
static const float kHot = 16.0;

float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
bool Finite(float3 c) { return all(abs(c) < 3.0e38); }  // false for NaN and Inf

void Measure(float3 input, float3 dlss, float3 clamped)
{
    const bool inOk = Finite(input), dlssOk = Finite(dlss);
    const float li = inOk ? max(Luma(input), 0.0) : 0.0;
    const float ld = dlssOk ? max(Luma(dlss), 0.0) : 0.0;
    const float lc = Finite(clamped) ? max(Luma(clamped), 0.0) : 0.0;
    InterlockedAdd(Stats[0], 1);
    InterlockedAdd(Stats[1], (uint)(min(li, kSumClip) * kSumScale));
    InterlockedAdd(Stats[2], (uint)(min(ld, kSumClip) * kSumScale));
    InterlockedAdd(Stats[3], (uint)(min(lc, kSumClip) * kSumScale));
    InterlockedMax(Stats[4], asuint(min(li, 3.0e38)));
    InterlockedMax(Stats[5], asuint(min(ld, 3.0e38)));
    InterlockedMax(Stats[6], asuint(min(lc, 3.0e38)));
    if (ld > lc * 1.25 + 0.01) InterlockedAdd(Stats[7], 1);
    if (!inOk) InterlockedAdd(Stats[8], 1);
    if (!dlssOk) InterlockedAdd(Stats[9], 1);
    if (ld > kHot) InterlockedAdd(Stats[10], 1);
    if (li > kHot) InterlockedAdd(Stats[11], 1);
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= (uint2)OutSize))
        return;

    // The render pixel under this output pixel's centre.
    const int2 center = (int2)(((float2)id + 0.5) * RenderSize / OutSize);
    const int2 last = (int2)RenderSize - 1;

    float3 lo = 1e30, hi = -1e30;
    [unroll] for (int y = -1; y <= 1; ++y)
    {
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            const float3 c = InputColor.Load(int3(clamp(center + int2(x, y), 0, last), 0)).rgb;
            lo = min(lo, c);
            hi = max(hi, c);
        }
    }

    const float4 d = DlssOutput[id];
    const float3 clamped = clamp(d.rgb, lo, hi);
    Clamped[id] = float4(clamped, d.a);

    if (all((id & 3) == 0))
        Measure(InputColor.Load(int3(clamp(center, 0, last), 0)).rgb, d.rgb, clamped);
}
