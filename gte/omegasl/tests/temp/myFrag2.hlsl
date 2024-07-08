struct VertexRaster2{
    float4 pos:SV_Position;
    float2 coord:TEXCOORD;
};

Texture2D myTexture: register(t0,space1);
SamplerState mySampler: register(s0,space1);
float4 myFrag2(VertexRaster2 raster) : SV_TARGET{
  return myTexture.Sample(mySampler,raster.coord);
}
