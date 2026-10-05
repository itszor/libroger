/* Display shaders: RetroArch slang presets (compiled by shaders/build.py into
 * shader_chains.h tables) run on SDL_GPU, the device of SDL's GPU renderer.
 *
 * Each frame, in one command buffer of our own: the game's screen (one texel
 * per pixel) is uploaded as "Original", every pass of the chain renders a
 * quad into its output texture (sized from its source, the viewport or
 * absolute, as the preset says) with its own vertex and fragment shaders,
 * mipmaps are generated where a later pass wants them, and the last pass
 * draws into a viewport-sized texture that host.c then draws 1:1 into the
 * window through the renderer (whose command buffer is submitted after
 * ours). Feedback (a pass's previous output) and history (previous
 * frames) are kept by swapping textures.
 *
 * Uniforms: each pass has one uniform block (vertex set 1 / fragment set 3,
 * binding 0) whose members build.py described: sizes, frame count,
 * parameters. Samplers: fragment set 2 (binding 0 = Source), vertex set 0. */
#include <SDL3/SDL.h>
#include <math.h>
#include <string.h>
#include "internal.h"
#include "shader_chains.h"

#define MAXPASS 32
#define MAXHIST 8
#define MAXLUT  16

struct target { SDL_GPUTexture *t; int w, h, levels; SDL_GPUTextureFormat fmt; };

static SDL_GPUDevice *dev;
static SDL_Renderer *ren;
static const struct rs_chain *ch;
static struct pass {
    SDL_GPUShader *vs, *fs;
    SDL_GPUGraphicsPipeline *pipe;
    SDL_GPUTextureFormat fmt;
    struct target out, fb;
    int w, h, sw, sh;                         /* output and source sizes this frame */
} P[MAXPASS];
static struct target orig, hist[MAXHIST], luts[MAXLUT], final_tgt;
static SDL_Texture *final_tex;                /* final_tgt wrapped for the renderer */
static int final_w, final_h;
static SDL_GPUBuffer *vbuf;
static SDL_GPUTransferBuffer *upload;
static uint32_t upload_size;
static SDL_GPUSampler *samplers[2][4][2];     /* [linear][wrap][mipmap] */
static uint8_t need_mips[5][MAXPASS];         /* [texkind][index] */
static uint32_t frame;

/* ---------------------------------------------------------------- resources */
static SDL_GPUTextureFormat gpu_format(int f)
{
    switch (f) {
    case RS_SRGB8: return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;
    case RS_RGBA16F: return SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    case RS_RGBA32F: return SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
    default: return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    }
}

static int mip_levels(int w, int h)
{
    int n = 1;
    while ((w | h) >> n) n++;
    return n;
}

static void free_target(struct target *t)
{
    if (t->t) SDL_ReleaseGPUTexture(dev, t->t);
    memset(t, 0, sizeof *t);
}

/* (re)create t as w x h of fmt (with mipmaps if mips); 0 on failure */
static int ensure_target(struct target *t, int w, int h, SDL_GPUTextureFormat fmt, int mips)
{
    int levels = mips ? mip_levels(w, h) : 1;
    if (t->t && t->w == w && t->h == h && t->fmt == fmt && t->levels == levels) return 1;
    free_target(t);
    SDL_GPUTextureCreateInfo ci;
    SDL_zero(ci);
    ci.type = SDL_GPU_TEXTURETYPE_2D;
    ci.format = fmt;
    ci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    ci.width = (Uint32)w; ci.height = (Uint32)h;
    ci.layer_count_or_depth = 1;
    ci.num_levels = (Uint32)levels;
    t->t = SDL_CreateGPUTexture(dev, &ci);
    if (!t->t) { SDL_Log("shader: texture %dx%d: %s", w, h, SDL_GetError()); return 0; }
    t->w = w; t->h = h; t->fmt = fmt; t->levels = levels;
    return 1;
}

static SDL_GPUSampler *sampler(int linear, int wrap, int mip)
{
    SDL_GPUSampler **s = &samplers[!!linear][wrap & 3][!!mip];
    if (*s) return *s;
    SDL_GPUSamplerCreateInfo ci;
    SDL_zero(ci);
    ci.min_filter = ci.mag_filter = linear ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    ci.mipmap_mode = linear ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    /* SDL_GPU has no border colour: clamp_to_border is taken as clamp_to_edge */
    SDL_GPUSamplerAddressMode m = wrap == RS_REPEAT ? SDL_GPU_SAMPLERADDRESSMODE_REPEAT
                                : wrap == RS_MIRROR ? SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT
                                : SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    ci.address_mode_u = ci.address_mode_v = ci.address_mode_w = m;
    ci.min_lod = 0; ci.max_lod = mip ? 1000.0f : 0.0f;
    *s = SDL_CreateGPUSampler(dev, &ci);
    return *s;
}

static SDL_GPUShader *make_shader(const uint8_t *code, size_t len, SDL_GPUShaderStage stage,
                                  int nsamplers, int ubo)
{
    SDL_GPUShaderCreateInfo ci;
    SDL_zero(ci);
    ci.code = code; ci.code_size = len;
    ci.entrypoint = "main";
    ci.format = SDL_GPU_SHADERFORMAT_SPIRV;
    ci.stage = stage;
    ci.num_samplers = (Uint32)nsamplers;
    ci.num_uniform_buffers = ubo ? 1 : 0;
    return SDL_CreateGPUShader(dev, &ci);
}

/* upload RGBA8 pixels into level 0 of t (in its own copy pass of cmd) */
static void upload_pixels(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *t, const void *pix, int w, int h)
{
    uint32_t size = (uint32_t)w * (uint32_t)h * 4;
    if (size > upload_size) {
        if (upload) SDL_ReleaseGPUTransferBuffer(dev, upload);
        SDL_GPUTransferBufferCreateInfo ti = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, size, 0};
        upload = SDL_CreateGPUTransferBuffer(dev, &ti);
        upload_size = upload ? size : 0;
        if (!upload) return;
    }
    void *map = SDL_MapGPUTransferBuffer(dev, upload, true);
    if (!map) return;
    memcpy(map, pix, size);
    SDL_UnmapGPUTransferBuffer(dev, upload);
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src = {upload, 0, (Uint32)w, (Uint32)h};
    SDL_GPUTextureRegion dst;
    SDL_zero(dst);
    dst.texture = t; dst.w = (Uint32)w; dst.h = (Uint32)h; dst.d = 1;
    SDL_UploadToGPUTexture(cp, &src, &dst, false);
    SDL_EndGPUCopyPass(cp);
}

static void free_chain(void)
{
    for (int i = 0; i < MAXPASS; i++) {
        if (P[i].pipe) SDL_ReleaseGPUGraphicsPipeline(dev, P[i].pipe);
        if (P[i].vs) SDL_ReleaseGPUShader(dev, P[i].vs);
        if (P[i].fs) SDL_ReleaseGPUShader(dev, P[i].fs);
        free_target(&P[i].out);
        free_target(&P[i].fb);
    }
    memset(P, 0, sizeof P);
    for (int i = 0; i < MAXHIST; i++) free_target(&hist[i]);
    for (int i = 0; i < MAXLUT; i++) free_target(&luts[i]);
    free_target(&orig);
    ch = NULL;
}

/* ---------------------------------------------------------------- set-up */
int shader_init(SDL_Renderer *r)
{
    ren = r;
    dev = SDL_GetGPURendererDevice(r);
    if (!dev) return 0;
    if (!(SDL_GetGPUShaderFormats(dev) & SDL_GPU_SHADERFORMAT_SPIRV)) {
        SDL_Log("shaders: the GPU device takes no SPIR-V");
        dev = NULL;
        return 0;
    }
    /* the quad: Position (0..1, as RetroArch's) and TexCoord */
    static const float quad[4][6] = {{0, 0, 0, 1, 0, 0}, {1, 0, 0, 1, 1, 0}, {0, 1, 0, 1, 0, 1}, {1, 1, 0, 1, 1, 1}};
    SDL_GPUBufferCreateInfo bi = {SDL_GPU_BUFFERUSAGE_VERTEX, sizeof quad, 0};
    vbuf = SDL_CreateGPUBuffer(dev, &bi);
    SDL_GPUTransferBufferCreateInfo ti = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, sizeof quad, 0};
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(dev, &ti);
    if (!vbuf || !tb) { dev = NULL; return 0; }
    memcpy(SDL_MapGPUTransferBuffer(dev, tb, false), quad, sizeof quad);
    SDL_UnmapGPUTransferBuffer(dev, tb);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation src = {tb, 0};
    SDL_GPUBufferRegion dst = {vbuf, 0, sizeof quad};
    SDL_UploadToGPUBuffer(cp, &src, &dst, false);
    SDL_EndGPUCopyPass(cp);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(dev, tb);
    return 1;
}

int shader_count(void) { return dev ? rs_nchains : 0; }

void shader_list(void)
{
    printf("none\n");
    for (int i = 0; i < rs_nchains; i++) printf("%s (%d pass%s)\n", rs_chains[i].name, rs_chains[i].npasses,
                                                rs_chains[i].npasses == 1 ? "" : "es");
}
const char *shader_name(int i) { return i >= 0 && i < rs_nchains ? rs_chains[i].name : "nearest"; }

int shader_find(const char *name)
{
    for (int i = 0; i < rs_nchains; i++)
        if (!SDL_strcasecmp(rs_chains[i].name, name)) return i;
    return -1;
}

static void mark_mips(const struct rs_sampler *s, int n)
{
    for (int k = 0; k < n; k++)
        if (s[k].mipmap) need_mips[s[k].tex.kind][s[k].tex.index] = 1;
}

/* make chain i current (-1: none); 0 if it can't be */
int shader_select(int i)
{
    free_chain();
    if (i < 0 || i >= rs_nchains || !dev) return i < 0;
    const struct rs_chain *c = &rs_chains[i];
    if (c->npasses > MAXPASS || c->history > MAXHIST || c->nluts > MAXLUT) return 0;
    memset(need_mips, 0, sizeof need_mips);
    for (int p = 0; p < c->npasses; p++) {
        const struct rs_pass *ps = &c->passes[p];
        mark_mips(ps->fsamplers, ps->nfsamplers);
        mark_mips(ps->vsamplers, ps->nvsamplers);
        if (ps->mipmap_input) need_mips[p ? RS_TX_OUTPUT : RS_TX_ORIGINAL][p ? p - 1 : 0] = 1;
    }
    for (int p = 0; p < c->npasses; p++) {
        const struct rs_pass *ps = &c->passes[p];
        int last = p == c->npasses - 1;
        int direct = last && ps->scale_type[0] == RS_SCALE_VIEWPORT && ps->scale_type[1] == RS_SCALE_VIEWPORT &&
                     ps->scale[0] == 1.0f && ps->scale[1] == 1.0f;
        P[p].fmt = direct ? SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM : gpu_format(ps->format);
        P[p].vs = make_shader(ps->vert, ps->vert_len, SDL_GPU_SHADERSTAGE_VERTEX, ps->nvsamplers, ps->ubo_size != 0);
        P[p].fs = make_shader(ps->frag, ps->frag_len, SDL_GPU_SHADERSTAGE_FRAGMENT, ps->nfsamplers, ps->ubo_size != 0);
        if (!P[p].vs || !P[p].fs) { SDL_Log("shader %s pass %d: %s", c->name, p, SDL_GetError()); free_chain(); return 0; }
        SDL_GPUVertexBufferDescription vb = {0, 6 * sizeof(float), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0};
        SDL_GPUVertexAttribute va[2] = {{0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0},
                                        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 4 * sizeof(float)}};
        SDL_GPUColorTargetDescription ct;
        SDL_zero(ct);
        ct.format = P[p].fmt;
        SDL_GPUGraphicsPipelineCreateInfo pi;
        SDL_zero(pi);
        pi.vertex_shader = P[p].vs;
        pi.fragment_shader = P[p].fs;
        pi.vertex_input_state.vertex_buffer_descriptions = &vb;
        pi.vertex_input_state.num_vertex_buffers = 1;
        pi.vertex_input_state.vertex_attributes = va;
        pi.vertex_input_state.num_vertex_attributes = 2;
        pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
        pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        pi.target_info.color_target_descriptions = &ct;
        pi.target_info.num_color_targets = 1;
        P[p].pipe = SDL_CreateGPUGraphicsPipeline(dev, &pi);
        if (!P[p].pipe) { SDL_Log("shader %s pass %d: pipeline: %s", c->name, p, SDL_GetError()); free_chain(); return 0; }
    }
    /* lookup textures */
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    for (int l = 0; l < c->nluts; l++) {
        const struct rs_lut *lt = &c->luts[l];
        SDL_Surface *s = SDL_LoadPNG_IO(SDL_IOFromConstMem(lt->png, lt->png_len), true);
        SDL_Surface *rgba = s ? SDL_ConvertSurface(s, SDL_PIXELFORMAT_ABGR8888) : NULL;
        if (!rgba || !ensure_target(&luts[l], rgba->w, rgba->h, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, lt->mipmap)) {
            SDL_Log("shader %s: LUT %s: %s", c->name, lt->name, SDL_GetError());
        } else {
            if (rgba->pitch == rgba->w * 4) upload_pixels(cmd, luts[l].t, rgba->pixels, rgba->w, rgba->h);
            if (lt->mipmap) SDL_GenerateMipmapsForGPUTexture(cmd, luts[l].t);
        }
        SDL_DestroySurface(rgba);
        SDL_DestroySurface(s);
    }
    SDL_SubmitGPUCommandBuffer(cmd);
    ch = c;
    frame = 0;
    return 1;
}

/* ---------------------------------------------------------------- a frame */
static struct target *tex_of(struct rs_texref r)
{
    switch (r.kind) {
    case RS_TX_ORIGINAL: return &orig;
    case RS_TX_HISTORY: return &hist[r.index];
    case RS_TX_OUTPUT: return &P[r.index].out;
    case RS_TX_FEEDBACK: return P[r.index].fb.t ? &P[r.index].fb : &P[r.index].out;
    default: return &luts[r.index];
    }
}

static void put_vec4(uint8_t *u, float w, float h)
{
    float v[4] = {w, h, w ? 1.0f / w : 0, h ? 1.0f / h : 0};
    memcpy(u, v, sizeof v);
}

static void put_scalar(uint8_t *u, int type, double v)
{
    if (type == RS_T_FLOAT) { float f = (float)v; memcpy(u, &f, 4); }
    else { int32_t n = (int32_t)v; memcpy(u, &n, 4); }
}

static void fill_uniforms(int p, uint8_t *u, int vw, int vh)
{
    const struct rs_pass *ps = &ch->passes[p];
    memset(u, 0, ps->ubo_size);
    for (const struct rs_uniform *x = ps->uniforms; x->kind != RS_U_END; x++) {
        uint8_t *d = u + x->offset;
        switch (x->kind) {
        case RS_U_MVP: {                        /* (0..1) -> clip space, y down the target */
            static const float m[16] = {2, 0, 0, 0, 0, -2, 0, 0, 0, 0, 1, 0, -1, 1, 0, 1};
            memcpy(d, m, sizeof m);
            break;
        }
        case RS_U_SOURCE_SIZE: put_vec4(d, (float)P[p].sw, (float)P[p].sh); break;
        case RS_U_ORIGINAL_SIZE: put_vec4(d, (float)orig.w, (float)orig.h); break;
        case RS_U_OUTPUT_SIZE: put_vec4(d, (float)P[p].w, (float)P[p].h); break;
        case RS_U_VIEWPORT_SIZE: put_vec4(d, (float)vw, (float)vh); break;
        case RS_U_FRAME_COUNT: put_scalar(d, x->type, ps->frame_count_mod ? frame % ps->frame_count_mod : frame); break;
        case RS_U_FRAME_DIRECTION: case RS_U_ONE: put_scalar(d, x->type, 1); break;
        case RS_U_ASPECT: put_scalar(d, x->type, 4.0 / 3.0); break;      /* the Archimedes' monitor */
        case RS_U_FPS: put_scalar(d, x->type, 50); break;
        case RS_U_FRAME_TIME: put_scalar(d, x->type, 20000); break;
        case RS_U_TEX_SIZE: { struct target *t = tex_of(x->tex); put_vec4(d, (float)t->w, (float)t->h); break; }
        case RS_U_PARAM: put_scalar(d, x->type, x->value); break;
        default: break;
        }
    }
}

static int scaled(int type, float scale, int src, int view)
{
    float v = type == RS_SCALE_SOURCE ? src * scale : type == RS_SCALE_VIEWPORT ? view * scale : scale;
    int n = (int)(v + 0.5f);
    return n < 1 ? 1 : n;
}

static void bind_samplers(SDL_GPURenderPass *rp, const struct rs_sampler *s, int n, int vertex)
{
    SDL_GPUTextureSamplerBinding b[16];
    for (int k = 0; k < n && k < 16; k++) {
        b[k].texture = tex_of(s[k].tex)->t;
        b[k].sampler = sampler(s[k].linear, s[k].wrap, s[k].mipmap);
    }
    if (n > 16) n = 16;
    if (vertex) SDL_BindGPUVertexSamplers(rp, 0, b, (Uint32)n);
    else SDL_BindGPUFragmentSamplers(rp, 0, b, (Uint32)n);
}

/* Run the chain over the game's screen (w x h RGBA8 pixels) for a viewport
 * of vw x vh output pixels; the result to draw, or NULL. */
SDL_Texture *shader_run(const uint32_t *pix, int w, int h, int vw, int vh)
{
    if (!ch) return NULL;
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    if (!cmd) return NULL;
    /* sizes, and the textures they need */
    int ok = ensure_target(&orig, w, h, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, need_mips[RS_TX_ORIGINAL][0]);
    for (int k = 0; k < ch->history; k++)
        ok &= ensure_target(&hist[k], w, h, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, need_mips[RS_TX_HISTORY][k]);
    int sw = w, sh = h;
    for (int p = 0; p < ch->npasses; p++) {
        const struct rs_pass *ps = &ch->passes[p];
        P[p].sw = sw; P[p].sh = sh;
        P[p].w = scaled(ps->scale_type[0], ps->scale[0], sw, vw);
        P[p].h = scaled(ps->scale_type[1], ps->scale[1], sh, vh);
        int mips = need_mips[RS_TX_OUTPUT][p];
        ok &= ensure_target(&P[p].out, P[p].w, P[p].h, P[p].fmt, mips);
        if (ch->feedback & (1u << p)) ok &= ensure_target(&P[p].fb, P[p].w, P[p].h, P[p].fmt, mips);
        sw = P[p].w; sh = P[p].h;
    }
    ok &= ensure_target(&final_tgt, vw, vh, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, 0);
    if (!ok) { SDL_CancelGPUCommandBuffer(cmd); return NULL; }

    upload_pixels(cmd, orig.t, pix, w, h);
    if (orig.levels > 1) SDL_GenerateMipmapsForGPUTexture(cmd, orig.t);
    static uint8_t ubo[4096];
    for (int p = 0; p < ch->npasses; p++) {
        const struct rs_pass *ps = &ch->passes[p];
        SDL_GPUColorTargetInfo ct;
        SDL_zero(ct);
        ct.texture = P[p].out.t;
        ct.load_op = SDL_GPU_LOADOP_CLEAR;
        ct.store_op = SDL_GPU_STOREOP_STORE;
        ct.cycle = true;
        if (ps->ubo_size && ps->ubo_size <= sizeof ubo) {
            fill_uniforms(p, ubo, vw, vh);
            SDL_PushGPUVertexUniformData(cmd, 0, ubo, ps->ubo_size);
            SDL_PushGPUFragmentUniformData(cmd, 0, ubo, ps->ubo_size);
        }
        SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cmd, &ct, 1, NULL);
        SDL_BindGPUGraphicsPipeline(rp, P[p].pipe);
        SDL_GPUBufferBinding vb = {vbuf, 0};
        SDL_BindGPUVertexBuffers(rp, 0, &vb, 1);
        bind_samplers(rp, ps->fsamplers, ps->nfsamplers, 0);
        if (ps->nvsamplers) bind_samplers(rp, ps->vsamplers, ps->nvsamplers, 1);
        SDL_DrawGPUPrimitives(rp, 4, 1, 0, 0);
        SDL_EndGPURenderPass(rp);
        if (P[p].out.levels > 1) SDL_GenerateMipmapsForGPUTexture(cmd, P[p].out.t);
    }
    /* the last output, scaled to the viewport */
    struct target *last = &P[ch->npasses - 1].out;
    SDL_GPUBlitInfo bl;
    SDL_zero(bl);
    bl.source.texture = last->t; bl.source.w = (Uint32)last->w; bl.source.h = (Uint32)last->h;
    bl.destination.texture = final_tgt.t; bl.destination.w = (Uint32)vw; bl.destination.h = (Uint32)vh;
    bl.load_op = SDL_GPU_LOADOP_DONT_CARE;
    bl.filter = SDL_GPU_FILTER_LINEAR;
    bl.cycle = true;
    SDL_BlitGPUTexture(cmd, &bl);
    /* history and feedback for the next frame */
    if (ch->history) {
        struct target t = hist[ch->history - 1];
        memmove(hist + 1, hist, (size_t)(ch->history - 1) * sizeof *hist);
        hist[0] = t;
        SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureLocation src = {orig.t, 0, 0, 0, 0, 0}, dst = {hist[0].t, 0, 0, 0, 0, 0};
        SDL_CopyGPUTextureToTexture(cp, &src, &dst, (Uint32)w, (Uint32)h, 1, false);
        SDL_EndGPUCopyPass(cp);
    }
    for (int p = 0; p < ch->npasses; p++)
        if (ch->feedback & (1u << p)) { struct target t = P[p].fb; P[p].fb = P[p].out; P[p].out = t; }
    SDL_SubmitGPUCommandBuffer(cmd);
    frame++;

    if (!final_tex || final_w != vw || final_h != vh) {
        if (final_tex) SDL_DestroyTexture(final_tex);
        SDL_PropertiesID pr = SDL_CreateProperties();
        SDL_SetNumberProperty(pr, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, SDL_PIXELFORMAT_ABGR8888);
        SDL_SetNumberProperty(pr, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STATIC);
        SDL_SetNumberProperty(pr, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, vw);
        SDL_SetNumberProperty(pr, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, vh);
        SDL_SetPointerProperty(pr, SDL_PROP_TEXTURE_CREATE_GPU_TEXTURE_POINTER, final_tgt.t);
        final_tex = SDL_CreateTextureWithProperties(ren, pr);
        SDL_DestroyProperties(pr);
        if (final_tex) SDL_SetTextureScaleMode(final_tex, SDL_SCALEMODE_NEAREST);
        final_w = vw; final_h = vh;
    }
    return final_tex;
}
