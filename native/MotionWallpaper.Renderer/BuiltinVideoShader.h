#pragma once

namespace motion::renderer {
    // YUV conversion, transfer and gamut conversion occur on the presentation
    // GPU, after the clock has selected a frame. Desktop output is sRGB SDR.
    inline constexpr char builtin_video_shader[] = R"hlsl(
Texture2D<float> Y : register(t0);
Texture2D<float2> UV : register(t1);
SamplerState sampleLinear : register(s0);
cbuffer Params : register(b0) {
    float4 crop;
    float4 range;
    float4 matrixYuv;
    float4 gamut0;
    float4 gamut1;
    float4 gamut2;
    float4 options; // transfer, clockwise rotation, texture width ratio, height ratio
    float4 sampling; // chroma offset x/y, source linear-light luma R/B weights
};
struct Vertex { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
Vertex vs(uint id : SV_VertexID) {
    Vertex v;
    float2 p = float2((id << 1) & 2, id & 2);
    v.position = float4(p * float2(2,-2) + float2(-1,1), 0, 1);
    v.uv = p;
    return v;
}
float3 linearize(float3 v) {
    v = max(v, 0);
    if (options.x == 4) return pow(v, 2.2);
    if (options.x == 5) return pow(v, 2.8);
    if (options.x == 8) return v;
    if (options.x == 13) return lerp(pow((v + .055) / 1.055, 2.4), v / 12.92, step(v, .04045));
    if (options.x == 16) {
        float3 p = pow(v, 32.0 / 2523.0);
        return 100 * pow(max(p - 3424.0/4096.0, 0) / max(2413.0/128.0 - 2392.0/128.0 * p, 1e-6), 16384.0/2610.0);
    }
    if (options.x == 18) {
        float3 scene = lerp((exp((v - .55991073) / .17883277) + .28466892) / 12, v*v/3, step(v,.5));
        return scene * pow(max(dot(scene,float3(sampling.z,1-sampling.z-sampling.w,sampling.w)), 1e-6), .2) * 10;
    }
    // Display-referred BT.709 / BT.2020 SDR uses the BT.1886 EOTF.
    return pow(v,2.4);
}
float4 ps(Vertex v) : SV_TARGET {
    float2 p = lerp(crop.xy, crop.zw, v.uv);
    if (options.y == 90) p = float2(p.y,1-p.x);
    else if (options.y == 180) p = 1-p;
    else if (options.y == 270) p = float2(1-p.y,p.x);
    p *= options.zw;
    float y = (Y.Sample(sampleLinear,p)-range.x)*range.y;
    float2 c = (UV.Sample(sampleLinear,p+sampling.xy)-range.z)*range.w;
    float3 rgb = float3(y+matrixYuv.x*c.y, y+matrixYuv.y*c.x+matrixYuv.z*c.y, y+matrixYuv.w*c.x);
    rgb = linearize(rgb);
    rgb = float3(dot(gamut0.xyz,rgb),dot(gamut1.xyz,rgb),dot(gamut2.xyz,rgb));
    if (options.x == 16 || options.x == 18) {
        float peak = max(gamut0.w,1);
        rgb *= (1+1/peak) / (1+max(max(rgb.r,rgb.g),rgb.b));
    }
    rgb = saturate(rgb);
    rgb = lerp(1.055*pow(rgb,1.0/2.4)-.055,12.92*rgb,step(rgb,.0031308));
    return float4(rgb,1);
}
)hlsl";
}
