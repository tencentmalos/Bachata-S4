#version 450
#extension GL_EXT_shader_explicit_arithmetic_types_int8 : require
#extension GL_EXT_shader_8bit_storage : require
#extension GL_EXT_shader_explicit_arithmetic_types_int16 : require
#extension GL_EXT_shader_16bit_storage : require
#if defined(GL_ARB_gpu_shader_int64)
#extension GL_ARB_gpu_shader_int64 : require
#else
#error No extension available for 64-bit integers.
#endif
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

struct full_result_i32x2
{
    int _m0;
    int _m1;
};

struct full_result_u32x2
{
    uint _m0;
    uint _m1;
};

struct frexp_result_f32
{
    float _m0;
    int _m1;
};

layout(set = 0, binding = 0, std430) readonly buffer ssbo_1
{
    uint data[];
} ssbo_1_1;

layout(set = 0, binding = 1, std430) readonly buffer ssbo_2
{
    uint data[];
} ssbo_2_1;

layout(set = 0, binding = 1, std430) readonly buffer ssbo_2_2
{
    float data[];
} ssbo_2_3;

layout(set = 0, binding = 1, std430) readonly buffer ssbo_2_4
{
    uint16_t data[];
} ssbo_2_5;

layout(set = 0, binding = 1, std430) readonly buffer ssbo_2_6
{
    uint8_t data[];
} ssbo_2_7;

layout(set = 0, binding = 2, std430) coherent buffer ssbo_3
{
    uint data[];
} ssbo_3_1;

layout(set = 0, binding = 2, std430) coherent buffer ssbo_3_2
{
    float data[];
} ssbo_3_3;

layout(set = 0, binding = 2, std430) coherent buffer ssbo_3_4
{
    uint16_t data[];
} ssbo_3_5;

layout(set = 0, binding = 2, std430) coherent buffer ssbo_3_6
{
    uint8_t data[];
} ssbo_3_7;

layout(push_constant, std430) uniform AuxData
{
    float xoffset;
    float yoffset;
    float xscale;
    float yscale;
    uvec4 ud_regs0;
    uvec4 ud_regs1;
    uvec4 ud_regs2;
    uvec4 ud_regs3;
    uvec4 buf_offsets0;
    uvec4 buf_offsets1;
    uvec2 buf_offsets2;
    uvec2 image_scales;
} push_data;

void main()
{
    uint _95 = (gl_WorkGroupID.x << 6u) + gl_LocalInvocationID.x;
    bool _96 = ssbo_1_1.data[0u] > _95;
    if (_96)
    {
        if (_96 && (push_data.ud_regs0.x > _95))
        {
            ssbo_3_3.data[_95] = vec4(vec4(vec4(vec4(ssbo_2_3.data[ssbo_1_1.data[1u] & _95], 0.0, 0.0, 0.0).x, vec4(0.0, 1.0, 0.0, 0.0).x, vec4(0.0, 1.0, 0.0, 0.0).x, vec4(0.0, 1.0, 0.0, 0.0).x).x, 0.0, 0.0, 0.0).x, vec4(0.0, 1.0, 0.0, 0.0).x, vec4(0.0, 1.0, 0.0, 0.0).x, vec4(0.0, 1.0, 0.0, 0.0).x).x;
        }
    }
}

