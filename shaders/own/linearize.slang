#version 450
// libroger: raise the input to a gamma (1.0: unchanged); stands in for
// slang-shaders' edge-smoothing/xbr/shaders/support/linearize.slang, with the
// same parameter.

layout(push_constant) uniform Push
{
    vec4 SourceSize;
    float InputGamma;
} params;

#pragma parameter InputGamma "INPUT GAMMA" 1.0 1.0 3.0 0.05

layout(std140, set = 0, binding = 0) uniform UBO
{
    mat4 MVP;
} global;

#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 vTexCoord;

void main()
{
    gl_Position = global.MVP * Position;
    vTexCoord = TexCoord;
}

#pragma stage fragment
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 2) uniform sampler2D Source;

void main()
{
    float InputGamma = params.InputGamma;
    vec3 c = texture(Source, vTexCoord).rgb;
    FragColor = vec4(pow(c, vec3(InputGamma)), 1.0);
}
