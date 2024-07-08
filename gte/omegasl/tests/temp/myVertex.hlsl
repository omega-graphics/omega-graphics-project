struct MyVertex{
    float4 pos;
    float4 color;
};

struct VertexRaster{
    float4 pos:SV_Position;
    float4 color:COLOR;
};

StructuredBuffer<MyVertex> v_buffer: register(t0,space0);
VertexRaster myVertex(uint vertex_id:SV_VertexID){
  MyVertex vert = v_buffer[vertex_id];
  VertexRaster data;
  data.pos = vert.pos;
  data.color = vert.color;
  return data;
}
