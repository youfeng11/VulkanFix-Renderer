#version 450
layout(location=0) flat out vec4 color;
void main() {
    const vec2 p[3] = vec2[3](vec2(-0.75,-0.75),vec2(0.75,-0.75),vec2(0,0.75));
    gl_Position = vec4(p[gl_VertexIndex].x, -p[gl_VertexIndex].y, 0, 1);
    // A viewport transform must not affect a subsequent Position read.
    gl_Position = vec4(gl_Position.x, -gl_Position.y, gl_Position.zw);
    color = vec4(0,1,0,1);
}
