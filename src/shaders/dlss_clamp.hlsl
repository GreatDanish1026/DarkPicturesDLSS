// Clamped copy of the DLSS output, for the game's screen-space lighting.
//
// The Quarry's SSGI and SSR (enabled by the pak mods) light each frame from the
// previous frame's TAA output. Under UE's TAA that loop is bounded because TAA clamps
// its history to the current frame's colours; DLSS has no such clamp, so errors fed back
// through the lighting grow into echoes and blown-out highlights. This pass writes the
// DLSS output clamped to the min/max of the current frame's input colour around each
// pixel (3x3, in render pixels); dlss.cpp copies it over the texture the lighting reads
// next frame. The displayed image stays the unclamped DLSS output.

cbuffer Constants : register(b0)
{
    float2 OutSize;     // output view rect, pixels (origin 0,0)
    float2 RenderSize;  // render view rect, pixels (origin 0,0)
};

Texture2D<float4> InputColor : register(t0);   // the TAAU's input colour (render res)
RWTexture2D<float4> DlssOutput : register(u0);  // the TAAU's output texture, just written by DLSS
RWTexture2D<float4> Clamped : register(u1);     // ours, same size as the output texture

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
    Clamped[id] = float4(clamp(d.rgb, lo, hi), d.a);
}
