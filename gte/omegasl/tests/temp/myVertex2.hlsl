struct MyVertex2{
    float4 pos;
    float2 coord;
};

struct VertexRaster2{
    float4 pos:SV_Position;
    float2 coord:TEXCOORD;
};

StructuredBuffer<MyVertex2> v_buffer_2: register(t0,space0);
VertexRaster2 myVertex2(uint v_id:SV_VertexID){
  MyVertex2 v = v_buffer_2[v_id];
  VertexRaster2 raster;
  raster.pos = v.pos;
  raster.coord = v.coord;
  return raster;
}
