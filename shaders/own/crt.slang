#version 450
// libroger's CRT shader (formerly shaders/crt.frag for --crt).
//
// Draws the game's 1:1 screen (320x256 or 640x256, already palette-converted
// RGB) as a monitor would: each source row is an electron beam with a
// gaussian vertical profile that widens with brightness, rows are sharply
// interpolated horizontally, and an aperture-grille mask is applied in
// output pixels. All blending is done in linear light.

layout(push_constant) uniform Push
{
    vec4 SourceSize;
    vec4 OutputSize;
} params;

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

const float GAMMA = 2.4;            // CRT-ish transfer
const float BEAM_MIN = 0.30;        // beam half-width (in rows) for dark pixels
const float BEAM_MAX = 0.48;        // ...and for bright ones (blooming)
const float SHARPNESS = 3.0;        // horizontal: 1 = bilinear, higher = sharper
const float MASK_DARK = 0.70;       // aperture grille: attenuation of the other phosphors
const float BRIGHTNESS = 1.30;      // compensates scanline + mask darkening

vec3 fetch_linear(vec2 texel)
{
    vec2 size = params.SourceSize.xy;
    vec2 uv = (clamp(texel, vec2(0.0), size - 1.0) + 0.5) / size;
    return pow(texture(Source, uv).rgb, vec3(GAMMA));
}

// one source row, sharply interpolated horizontally at source x position px
vec3 row_color(float px, float row)
{
    float x0 = floor(px - 0.5);
    float t = (px - 0.5) - x0;
    t = clamp((t - 0.5) * SHARPNESS + 0.5, 0.0, 1.0);
    return mix(fetch_linear(vec2(x0, row)), fetch_linear(vec2(x0 + 1.0, row)), t);
}

vec3 beam(vec3 c, float dist)
{
    vec3 w = mix(vec3(BEAM_MIN), vec3(BEAM_MAX), sqrt(c));      // brighter = wider
    return c * exp(-0.5 * (dist * dist) / (w * w));
}

void main()
{
    vec2 pos = vTexCoord * params.SourceSize.xy;
    float row = floor(pos.y);
    float dy = pos.y - (row + 0.5);                                 // -0.5..0.5 from row centre
    float nrow = row + (dy < 0.0 ? -1.0 : 1.0);                     // nearest neighbouring row

    vec3 c0 = row_color(pos.x, row);
    vec3 c1 = row_color(pos.x, nrow);
    vec3 col = beam(c0, dy) + beam(c1, 1.0 - abs(dy));

    // aperture grille, one phosphor triad per 3 output pixels
    float phase = mod(floor(gl_FragCoord.x), 3.0);
    vec3 mask = vec3(MASK_DARK);
    if (phase < 1.0) mask.r = 1.0; else if (phase < 2.0) mask.g = 1.0; else mask.b = 1.0;
    col *= mask * BRIGHTNESS;

    FragColor = vec4(pow(clamp(col, 0.0, 1.0), vec3(1.0 / GAMMA)), 1.0);
}
