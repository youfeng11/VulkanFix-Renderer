#version 450
layout(location=0) in float instanceValue;
layout(location=0) flat out vec4 color;
layout(push_constant) uniform Push { uint mode; } pc;
void main() {
    const vec2 quad[6] = vec2[6](vec2(-1,-1),vec2(1,-1),vec2(-1,1),vec2(-1,1),vec2(1,-1),vec2(1,1));
    const vec2 triangle[3] = vec2[3](vec2(-0.75,-0.75),vec2(0.75,-0.75),vec2(0,0.75));
    vec2 position = quad[gl_VertexIndex % 6];
    color = vec4(0,1,0,1);
    if (pc.mode == 1u) {
        bool right = gl_VertexIndex >= 6;
        position.x = position.x * 0.5 + (right ? 0.5 : -0.5);
        color = right ? vec4(0,1,0,1) : vec4(1,0,0,1);
    } else if (pc.mode == 2u) {
        color = gl_InstanceIndex == 7 ? vec4(0,1,0,1) : vec4(1,0,0,1);
    } else if (pc.mode == 3u) {
        position.x = position.x * 0.5 + (gl_InstanceIndex >= 2 ? 0.5 : -0.5);
        color = vec4(instanceValue,0,0,1);
    } else if (pc.mode == 4u) {
        position = triangle[gl_VertexIndex % 3];
    }
    gl_Position = vec4(position,0,1);
}
