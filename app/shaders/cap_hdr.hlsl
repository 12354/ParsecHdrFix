// Local host extension. FP16 duplication is linear Rec.709/scRGB, not PQ.
Texture2D<float4> desktop : register(t0);
cbuffer Parameters : register(b0) { float white; float rotation; float2 padding; };

float4 vs_main(uint id : SV_VertexID) : SV_Position
{
    return float4(id == 2 ? 3 : -1, id == 1 ? 3 : -1, 0, 1);
}

float encode_srgb(float v)
{
    return v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
}

float4 ps_main(float4 position : SV_Position) : SV_Target
{
    uint w, h; desktop.GetDimensions(w, h);
    int2 pixel = int2(position.xy);
    // Undo WGC's display orientation to honor DXGI duplication's raw-surface ABI.
    if (rotation == 2) pixel = int2(int(w) - 1 - pixel.y, pixel.x);
    else if (rotation == 3) pixel = int2(int(w) - 1 - pixel.x, int(h) - 1 - pixel.y);
    else if (rotation == 4) pixel = int2(pixel.y, int(h) - 1 - pixel.x);
    float3 rgb = desktop.Load(int3(pixel, 0)).rgb / max(white, 0.001);
    float luminance = max(dot(rgb, float3(0.2126, 0.7152, 0.0722)), 0);
    // Preserve shadows/midtones; smooth shoulder reserves room for HDR highlights.
    float mapped = luminance <= 0.75 ? luminance : 1 - 0.0625 / (luminance - 0.5);
    rgb *= mapped / max(luminance, 0.000001);
    // Compress chroma along the neutral->color ray to keep hue and luminance.
    float3 delta = rgb - mapped;
    float3 bounds = float3(1, 1, 1);
    [unroll] for (uint i = 0; i < 3; ++i) {
        if (delta[i] < 0) bounds[i] = mapped / -delta[i];
        else if (delta[i] > 0) bounds[i] = (1 - mapped) / delta[i];
    }
    rgb = saturate(mapped + delta * saturate(min(bounds.x, min(bounds.y, bounds.z))));
    return float4(encode_srgb(rgb.r), encode_srgb(rgb.g), encode_srgb(rgb.b), 1);
}
