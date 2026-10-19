#version 450

// A live image (a plugin's window) drawn pixel for pixel. Its pixels are sRGB; blending happens in
// linear space on an sRGB swapchain. The capture's alpha byte is unused: the image is opaque,
// faded by the instance's alpha.

layout(set = 0, binding = 0) uniform sampler2D image;

layout(location = 0) in vec2 vUv;
layout(location = 1) flat in vec4 vColor;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 rgb = texture(image, vUv).rgb;
    outColor = vec4(pow(rgb, vec3(2.2)), vColor.a);
}
