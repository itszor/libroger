/* libroger: display shader chains (RetroArch slang presets compiled by
 * shaders/build.py into shaders/generated/chains.c; run by shader.c). */
#ifndef ROGER_SHADER_CHAINS_H
#define ROGER_SHADER_CHAINS_H
#include <stddef.h>
#include <stdint.h>

enum rs_scale { RS_SCALE_SOURCE, RS_SCALE_VIEWPORT, RS_SCALE_ABSOLUTE };
enum rs_format { RS_RGBA8, RS_SRGB8, RS_RGBA16F, RS_RGBA32F };
enum rs_wrap { RS_CLAMP, RS_BORDER, RS_REPEAT, RS_MIRROR };
/* textures a pass can sample (and whose size it can ask for) */
enum rs_texkind { RS_TX_ORIGINAL, RS_TX_HISTORY, RS_TX_OUTPUT, RS_TX_FEEDBACK, RS_TX_LUT };
struct rs_texref { uint8_t kind, index; };          /* HISTORY index 0 = the previous frame */
enum rs_utype { RS_T_FLOAT, RS_T_INT, RS_T_UINT, RS_T_VEC2, RS_T_VEC3, RS_T_VEC4, RS_T_MAT4 };
enum rs_ukind { RS_U_END, RS_U_MVP, RS_U_SOURCE_SIZE, RS_U_ORIGINAL_SIZE, RS_U_OUTPUT_SIZE,
                RS_U_VIEWPORT_SIZE, RS_U_FRAME_COUNT, RS_U_FRAME_DIRECTION, RS_U_ZERO, RS_U_ONE,
                RS_U_ASPECT, RS_U_FPS, RS_U_FRAME_TIME, RS_U_TEX_SIZE, RS_U_PARAM };
/* one member of a pass's uniform block */
struct rs_uniform { uint16_t offset; uint8_t type, kind; struct rs_texref tex; float value; };
/* a sampler binding: what, and how it's sampled */
struct rs_sampler { struct rs_texref tex; uint8_t linear, wrap, mipmap; };
struct rs_pass {
    const uint8_t *vert; size_t vert_len;              /* SPIR-V */
    const uint8_t *frag; size_t frag_len;
    uint8_t scale_type[2]; float scale[2];             /* x, y */
    uint8_t linear, wrap, mipmap_input, format;        /* Source sampling; output format */
    uint32_t frame_count_mod;
    uint32_t ubo_size;
    const struct rs_uniform *uniforms;                 /* ends with RS_U_END */
    int nfsamplers; const struct rs_sampler *fsamplers;   /* binding 0 = Source */
    int nvsamplers; const struct rs_sampler *vsamplers;
};
struct rs_lut { const char *name; const uint8_t *png; size_t png_len; uint8_t linear, mipmap, wrap; };
struct rs_chain {
    const char *name;
    int npasses; const struct rs_pass *passes;
    int nluts; const struct rs_lut *luts;
    int history;                                       /* previous frames kept */
    uint32_t feedback;                                 /* passes whose last output is kept */
};
extern const struct rs_chain rs_chains[];
extern const int rs_nchains;
#endif
