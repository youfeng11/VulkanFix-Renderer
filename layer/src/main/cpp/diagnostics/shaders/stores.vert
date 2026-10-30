#version 450
layout(set=0,binding=0,std430) buffer Output { uint count; uint sum; uint values[3]; } result;
layout(location=0) flat out vec4 color;
void main() {
    const vec2 triangle[3] = vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));
    atomicAdd(result.count,1u);
    atomicAdd(result.sum,uint(gl_VertexIndex)+1u);
    result.values[gl_VertexIndex] = uint(gl_VertexIndex)+17u;
    gl_Position = vec4(triangle[gl_VertexIndex],0,1);
    color = vec4(0,1,0,1);
}
