#version 450
layout(set=0,binding=0) uniform sampler2D sourceImage;
layout(location=0) out vec4 outputColor;
void main() {
    outputColor = textureGrad(sourceImage,vec2(0.5),vec2(0.5,0),vec2(0,0.01));
}
