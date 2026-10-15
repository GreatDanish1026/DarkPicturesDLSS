// Turns Unreal 4.26's velocity buffer and depth into DLSS inputs, for the render
// view rect (top-left at 0,0 in the game's render targets):
//   OutMotion: motion vectors in render pixels, pointing from this frame's pixel to
//              where it was last frame (DLSS convention, MV scale 1).
//   OutDepth:  device depth as R32_FLOAT (DLSS can't read the game's R32G8X24 typeless).
//
// UE writes velocity only for moving objects (encoded, x > 0). Every other pixel moved
// only because the camera did, which View.ClipToPrevClip reprojects, the same way
// UE's TemporalAA.usf does. Both are screen-space (-1..1) current minus previous.

cbuffer Constants : register(b0)
{
    row_major float4x4 ClipToPrevClip;  // View.ClipToPrevClip, UE's row-vector convention
    float2 ViewSize;                     // render view rect, in pixels
    float2 InvViewSize;
};

Texture2D<float4> Velocity : register(t0);  // RGBA16_UNORM, UE EncodeVelocityToTexture
Texture2D<float> Depth : register(t1);      // R32_FLOAT_X8X24 view of the scene depth

RWTexture2D<float2> OutMotion : register(u0);
RWTexture2D<float> OutDepth : register(u1);

// Diagnostics, over every 4th pixel in x and y (cleared to 0 each frame, read back by
// dlss.cpp and logged): 0 sampled, 1 with object velocity, 2 moving more than 0.05 px,
// 3 sum of |motion| x4 (capped at 4096 px), 4 max |motion| x16, 5 depth == 0 (sky),
// 6 ~min depth bits (non-zero depths), 7 max depth bits.
RWBuffer<uint> Stats : register(u2);

float2 DecodeVelocity(float4 encoded)
{
    const float invDiv = 1.0 / (0.499 * 0.5);
    return encoded.xy * invDiv - 32767.0 / 65535.0 * invDiv;
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= (uint2)ViewSize))
        return;

    const float deviceZ = Depth[id];
    const float2 screenPos = ((float2)id + 0.5) * InvViewSize * float2(2.0, -2.0) + float2(-1.0, 1.0);

    float2 velocity;  // screen space, current - previous
    const float4 encoded = Velocity[id];
    if (encoded.x > 0.0)
    {
        velocity = DecodeVelocity(encoded);
    }
    else
    {
        const float4 prevClip = mul(float4(screenPos, deviceZ, 1.0), ClipToPrevClip);
        velocity = screenPos - prevClip.xy / prevClip.w;
    }

    // Screen space (y up, -1..1) -> pixels (y down), and flip to current -> previous.
    const float2 motion = -velocity * float2(0.5, -0.5) * ViewSize;
    OutMotion[id] = motion;
    OutDepth[id] = deviceZ;

    if ((id.x & 3) == 0 && (id.y & 3) == 0)
    {
        const float length = min(sqrt(dot(motion, motion)), 4096.0);
        InterlockedAdd(Stats[0], 1);
        if (encoded.x > 0.0)
            InterlockedAdd(Stats[1], 1);
        if (length > 0.05)
            InterlockedAdd(Stats[2], 1);
        InterlockedAdd(Stats[3], (uint)(length * 4.0));
        InterlockedMax(Stats[4], (uint)(length * 16.0));
        if (deviceZ == 0.0)
        {
            InterlockedAdd(Stats[5], 1);
        }
        else
        {
            InterlockedMax(Stats[6], ~asuint(deviceZ));
            InterlockedMax(Stats[7], asuint(deviceZ));
        }
    }
}
