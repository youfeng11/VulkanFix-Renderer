#version 450
#extension GL_ARB_shader_draw_parameters : require
layout(location=0) flat out vec4 color;
layout(push_constant) uniform Push { uint mode; } pc;
void main() {
    const vec2 quad[6] = vec2[6](vec2(-1,-1),vec2(1,-1),vec2(-1,1),vec2(-1,1),vec2(1,-1),vec2(1,1));
    vec2 position = quad[gl_VertexIndex % 6];
    bool right = gl_VertexIndex >= 6;
    int base = pc.mode == 1u ? (right ? 7 : 3) : 7;
    int draw = pc.mode == 1u && right ? 1 : 0;
    bool correct = gl_BaseInstanceARB == base && gl_InstanceIndex == base && gl_DrawIDARB == draw;
    if (pc.mode == 1u) position.x = position.x * 0.5 + (right ? 0.5 : -0.5);
    gl_Position = vec4(position,0,1);
    color = correct ? vec4(0,1,0,1) : vec4(1,0,0,1);
}
