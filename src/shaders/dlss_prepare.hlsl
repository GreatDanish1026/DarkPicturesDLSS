// Turns Unreal 4.26's velocity buffer and depth into DLSS inputs, for the render
// view rect (top-left at 0,0 in the game's render targets):
//   OutMotion: motion vectors in render pixels, pointing from this frame's pixel to
//              where it was last frame (DLSS convention, MV scale 1).
//   OutDepth:  device depth as R32_FLOAT (DLSS can't read the game's R32G8X24 typeless).
//   OutMask:   DLSS's "bias current colour" mask: 1 where a moving object was in the last
//              few frames but isn't any more. DLSS model J keeps the object's old colour
//              there when the background is dark and still (striped trails behind a fast
//              hand in The Devil in Me); the mask tells it to take the current frame.
//   OutTrail:  per pixel, how recently a moving object covered it (1 = this frame, minus
//              Trail.y per frame), read back next frame as PrevTrail.
//
// UE writes velocity only for moving objects (encoded, x > 0). Every other pixel moved
// only because the camera did, which View.ClipToPrevClip reprojects, the same way
// UE's TemporalAA.usf does. Both are screen-space (-1..1) current minus previous.

cbuffer Constants : register(b0)
{
    row_major float4x4 ClipToPrevClip;  // View.ClipToPrevClip, UE's row-vector convention
    float2 ViewSize;                     // render view rect, in pixels
    float2 InvViewSize;
    float4 Flags;                        // x: 1 = ignore the game's velocity (camera motion only)
                                         // y: > 0 = use camera motion where the game's velocity
                                         //    differs from it by more than y pixels (after cuts)
    float4 Trail;                        // x: > 0 = trail mask on; an object pixel is moving when
                                         //    its velocity differs from camera motion by more
                                         //    than x pixels. y: fade per frame. z: 1 = forget
                                         //    the previous trail (camera cut, size change)
};

Texture2D<float4> Velocity : register(t0);  // RGBA16_UNORM, UE EncodeVelocityToTexture
Texture2D<float> Depth : register(t1);      // R32_FLOAT_X8X24 view of the scene depth
Texture2D<float> PrevTrail : register(t2);  // R8_UNORM, last frame's OutTrail

RWTexture2D<float2> OutMotion : register(u0);
RWTexture2D<float> OutDepth : register(u1);

// Diagnostics, over every 4th pixel in x and y (cleared to 0 each frame, read back by
// dlss.cpp and logged): 0 sampled, 1 with object velocity, 2 moving more than 0.05 px,
// 3 sum of |motion| x4 (capped at 4096 px), 4 max |motion| x16, 5 depth == 0 (sky),
// 6 ~min depth bits (non-zero depths), 7 max depth bits. Over object-velocity pixels:
// 8 sum of |decoded velocity| x16 px, 9 sum of |camera reprojection| x16 px (what the
// pixel would move if it were static), 10 decoded > 4x camera + 1 px, 11 rejected by
// Flags.y (camera motion used instead), 12 in the trail mask, 13 moving object pixels.
RWBuffer<uint> Stats : register(u2);
RWTexture2D<float> OutTrail : register(u3);  // R8_UNORM
RWTexture2D<float> OutMask : register(u4);   // R8_UNORM, render resolution

float2 DecodeVelocity(float4 encoded)
{
    const float invDiv = 1.0 / (0.499 * 0.5);
    return encoded.xy * invDiv - 32767.0 / 65535.0 * invDiv;
}

// Screen space (y up, -1..1, current - previous) -> DLSS pixels (y down, current -> previous).
float2 ToPixels(float2 velocity)
{
    return -velocity * float2(0.5, -0.5) * ViewSize;
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= (uint2)ViewSize))
        return;

    const float deviceZ = Depth[id];
    const float2 screenPos = ((float2)id + 0.5) * InvViewSize * float2(2.0, -2.0) + float2(-1.0, 1.0);

    const float4 prevClip = mul(float4(screenPos, deviceZ, 1.0), ClipToPrevClip);
    const float2 camera = ToPixels(screenPos - prevClip.xy / prevClip.w);

    // With Flags.x set the Velocity slot may hold another texture (the game bound a 1x1
    // placeholder, so dlss.cpp binds the depth there): don't read it as velocity.
    const float4 encoded = Velocity[id];
    const bool hasObject = Flags.x == 0.0 && encoded.x > 0.0;
    const float2 decoded = hasObject ? ToPixels(DecodeVelocity(encoded)) : float2(0.0, 0.0);
    const bool stale = hasObject && Flags.y > 0.0 && length(decoded - camera) > Flags.y;
    const float2 motion = (hasObject && Flags.x == 0.0 && !stale) ? decoded : camera;

    OutMotion[id] = motion;
    OutDepth[id] = deviceZ;

    // Trail mask. Where this pixel was last frame (motion points there), the most recent
    // coverage by a moving object in a 3x3 neighbourhood (covers rounding and thin edges).
    bool moving = false, masked = false;
    if (Trail.x > 0.0)
    {
        moving = hasObject && length(decoded - camera) > Trail.x;
        float recent = 0.0;
        if (Trail.z == 0.0)
        {
            const int2 prev = (int2)floor((float2)id + 0.5 + motion);
            const int2 last = (int2)ViewSize - 1;
            [unroll] for (int y = -1; y <= 1; ++y)
                [unroll] for (int x = -1; x <= 1; ++x)
                {
                    const int2 p = prev + int2(x, y);
                    if (all(p >= 0) && all(p <= last))
                        recent = max(recent, PrevTrail[p]);
                }
        }
        masked = !moving && recent > 0.1;
        OutTrail[id] = moving ? 1.0 : max(recent - Trail.y, 0.0);
        OutMask[id] = masked ? 1.0 : 0.0;
    }
    else
    {
        OutTrail[id] = 0.0;
        OutMask[id] = 0.0;
    }

    if ((id.x & 3) == 0 && (id.y & 3) == 0)
    {
        const float length = min(sqrt(dot(motion, motion)), 4096.0);
        InterlockedAdd(Stats[0], 1);
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
        if (hasObject)
        {
            const float decodedLength = min(sqrt(dot(decoded, decoded)), 4096.0);
            const float cameraLength = min(sqrt(dot(camera, camera)), 4096.0);
            InterlockedAdd(Stats[1], 1);
            InterlockedAdd(Stats[8], (uint)(decodedLength * 16.0));
            InterlockedAdd(Stats[9], (uint)(cameraLength * 16.0));
            if (decodedLength > 4.0 * cameraLength + 1.0)
                InterlockedAdd(Stats[10], 1);
            if (stale)
                InterlockedAdd(Stats[11], 1);
        }
        if (masked)
            InterlockedAdd(Stats[12], 1);
        if (moving)
            InterlockedAdd(Stats[13], 1);
    }
}
