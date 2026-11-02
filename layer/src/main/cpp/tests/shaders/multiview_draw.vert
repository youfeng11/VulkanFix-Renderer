#version 450
#extension GL_EXT_multiview : require
layout(set=0,binding=0,std140) uniform Tint { vec4 value; } tint;
layout(push_constant) uniform Push { vec4 value; } pc;
layout(location=0) flat out vec4 color;
void main() {
    const vec2 p[3] = vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));
    gl_Position = vec4(p[gl_VertexIndex],0,1);
    color = vec4(float(gl_ViewIndex)/255.0,tint.value.g,pc.value.b,1);
}
