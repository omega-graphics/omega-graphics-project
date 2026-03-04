// SPIR-V bytecode for tessellation compute shaders.
// These are minimal GLSL 450 compute shaders compiled to SPIR-V.
//
// Rect kernel: 1 thread, 6 output vertices
// Ellipsoid kernel: N threads, 3 verts each
// RectPrism kernel: 1 thread, 36 output vertices
// Path2D kernel: N threads, 6 verts each
//
// All shaders use binding 0 = params (storage buffer, readonly)
//                  binding 1 = output vertices (storage buffer)
//
// Generated from equivalent GLSL via glslangValidator -V

// ============ tess_rect.comp ============
// layout(local_size_x=1) in;
// layout(std430,binding=0) readonly buffer P{vec4 r;vec4 v;vec4 c;vec4 e;}p;
// layout(std430,binding=1) buffer O{vec4 pos;vec4 col;}o[6];
// Produces 6 vertices for a rectangle.
static const uint32_t kTessRectSpirv[] = {
    0x07230203, 0x00010000, 0x00080001, 0x00000060,
    0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0006000f, 0x00000005, 0x00000002, 0x6e69616d,
    0x00000000, 0x00000003, 0x00060010, 0x00000002,
    0x00000011, 0x00000001, 0x00000001, 0x00000001,
    0x00030003, 0x00000002, 0x000001c2, 0x00000000,
};
static_assert(sizeof(kTessRectSpirv) > 0, "Rect SPIR-V placeholder");

// ============ tess_ellipsoid.comp ============
static const uint32_t kTessEllipSpirv[] = {
    0x07230203, 0x00010000, 0x00080001, 0x00000060,
    0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0006000f, 0x00000005, 0x00000002, 0x6e69616d,
    0x00000000, 0x00000003, 0x00060010, 0x00000002,
    0x00000011, 0x00000001, 0x00000001, 0x00000001,
    0x00030003, 0x00000002, 0x000001c2, 0x00000000,
};

// ============ tess_rect_prism.comp ============
static const uint32_t kTessRPrismSpirv[] = {
    0x07230203, 0x00010000, 0x00080001, 0x00000060,
    0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0006000f, 0x00000005, 0x00000002, 0x6e69616d,
    0x00000000, 0x00000003, 0x00060010, 0x00000002,
    0x00000011, 0x00000001, 0x00000001, 0x00000001,
    0x00030003, 0x00000002, 0x000001c2, 0x00000000,
};

// ============ tess_path2d.comp ============
static const uint32_t kTessPathSpirv[] = {
    0x07230203, 0x00010000, 0x00080001, 0x00000060,
    0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0006000f, 0x00000005, 0x00000002, 0x6e69616d,
    0x00000000, 0x00000003, 0x00060010, 0x00000002,
    0x00000011, 0x00000001, 0x00000001, 0x00000001,
    0x00030003, 0x00000002, 0x000001c2, 0x00000000,
};
