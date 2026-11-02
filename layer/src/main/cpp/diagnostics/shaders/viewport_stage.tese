#version 450
layout(triangles, equal_spacing, ccw) in;
layout(location=0) flat out vec4 color;
void main() {
    // The evaluation shader owns final positions; VS positions are irrelevant.
    vec2 p = gl_TessCoord.x * vec2(-0.75,-0.75) +
             gl_TessCoord.y * vec2(0.75,-0.75) +
             gl_TessCoord.z * vec2(0,0.75);
    gl_Position = vec4(p.x, -p.y, 0, 1);
    gl_Position = vec4(gl_Position.x, -gl_Position.y, gl_Position.zw);
    color = vec4(0,1,0,1);
}
