struct VertexRaster{
    float4 pos:SV_Position;
    float4 color:COLOR;
};

float4 myFrag(VertexRaster raster) : SV_TARGET{
  return raster.color;
}
