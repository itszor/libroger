#!/usr/bin/env python3
"""Build libroger's display shaders: RetroArch slang presets -> SPIR-V and the
tables src/shader.c runs them from (shaders/generated/chains.c, committed, so
building libroger doesn't need this).

    build.py vendor DIR   copy the files the chains below need from a
                          checkout of https://github.com/libretro/slang-shaders
                          into shaders/slang/ (keeping their paths)
    build.py              compile the chains into shaders/generated/chains.c
                          (needs glslang: GLSLANG=... if not on PATH)

A slang shader is GLSL with both stages in one file (#pragma stage), its
parameters as #pragma parameter, and its uniforms in a push-constant block
and/or a UBO. For SDL_GPU each stage is compiled separately with one UBO
(vertex: set 1, fragment: set 3) holding every member of both blocks, and
with samplers renumbered (fragment: set 2, Source at binding 0 and the
others in order; vertex: set 0). The runtime fills the UBO from a table of
members and their meanings (sizes, frame count, parameters...) generated
here, and binds the samplers from a table of texture references.

Files that slang-shaders has without a licence and that are trivial are
replaced by libroger's own equivalents in shaders/own/ (REPLACE below).
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import slangp  # noqa: E402

SLANG = os.path.join(HERE, 'slang')
OWN = os.path.join(HERE, 'own')
OUT = os.path.join(HERE, 'generated', 'chains.c')

# (name, preset): cycled in this order after "nearest" (no shader)
CHAINS = [
    ('bilinear', 'own/bilinear.slangp'),
    ('lanczos3', 'slang/interpolation/lanczos3-fast.slangp'),
    ('spline36', 'slang/interpolation/spline36-fast.slangp'),
    ('xbr-lv2', 'slang/edge-smoothing/xbr/xbr-lv2.slangp'),
    ('xbrz', 'slang/edge-smoothing/xbrz/xbrz-freescale-multipass.slangp'),
    ('scalefx', 'slang/edge-smoothing/scalefx/scalefx.slangp'),
    ('crt', 'own/crt.slangp'),
    ('crt-lottes', 'slang/crt/crt-lottes.slangp'),
    ('crt-hyllian', 'slang/crt/crt-hyllian-fast.slangp'),
    ('crt-geom', 'slang/crt/crt-geom.slangp'),
    ('crt-easymode', 'slang/crt/crt-easymode.slangp'),
    ('crt-guest-advanced', 'slang/crt/crt-guest-advanced.slangp'),
    ('crt-royale', 'slang/crt/crt-royale.slangp'),
]

# slang-shaders files (relative to its root) replaced by our own
REPLACE = {
    'stock.slang': 'stock.slang',
    'crt/shaders/guest/advanced/stock.slang': 'stock.slang',
    'edge-smoothing/xbr/shaders/support/linearize.slang': 'linearize.slang',
    'edge-smoothing/xbr/shaders/support/delinearize.slang': 'delinearize.slang',
}


# ---------------------------------------------------------------- vendoring
def vendor(src):
    src = os.path.abspath(src)
    files = set()
    for name, preset in CHAINS:
        if not preset.startswith('slang/'):
            continue
        rel = preset[len('slang/'):]
        files.add(rel)
        pre = slangp.read_preset(os.path.join(src, rel))
        for k, v in pre[1].items():
            if re.fullmatch(r'shader\d+', k):
                r = os.path.relpath(v, src)
                if r in REPLACE:
                    continue
                for f in slangp.includes(v):
                    files.add(os.path.relpath(f, src))
        for t in pre[1].get('textures', '').split(';'):
            if t:
                files.add(os.path.relpath(pre[1][t], src))
    for f in sorted(files):
        d = os.path.join(SLANG, f)
        os.makedirs(os.path.dirname(d), exist_ok=True)
        shutil.copyfile(os.path.join(src, f), d)
    rev = subprocess.run(['git', '-C', src, 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
    with open(os.path.join(SLANG, 'UPSTREAM'), 'w') as f:
        f.write('https://github.com/libretro/slang-shaders %s\n' % rev)
    print('%d files copied into %s' % (len(files), SLANG))


# ---------------------------------------------------------------- conversion
BLOCK = re.compile(r'layout\s*\(([^)]*)\)\s*uniform\s+(\w+)\s*\{(.*?)\}\s*(\w+)\s*;', re.S)
SAMPLER = re.compile(r'(?:layout\s*\([^)]*\)\s*)?uniform\s+sampler2D\s+(\w+)\s*;')
SIZES = {'mat4': (64, 16), 'vec4': (16, 16), 'vec3': (12, 16), 'vec2': (8, 8),
         'float': (4, 4), 'int': (4, 4), 'uint': (4, 4), 'ivec4': (16, 16), 'uvec4': (16, 16)}


def strip_comments(s):
    s = re.sub(r'/\*.*?\*/', ' ', s, flags=re.S)
    return re.sub(r'//[^\n]*', '', s)


def convert(path):
    """one slang shader -> dict: vert/frag GLSL, params, alias, format,
    UBO members [(name, type)], fragment and vertex sampler names"""
    real = path
    if os.path.relpath(path, SLANG) in REPLACE:
        real = os.path.join(OWN, REPLACE[os.path.relpath(path, SLANG)])
    src = slangp.expand_includes(real)
    params, alias, fmt = {}, None, None
    lines = []
    for line in src.split('\n'):
        s = line.strip()
        m = re.match(r'#pragma\s+parameter\s+(\w+)\s+"[^"]*"\s+([-+.\deE]+)', s)
        if m:
            params.setdefault(m.group(1), float(m.group(2)))
            continue
        m = re.match(r'#pragma\s+name\s+(\w+)', s)
        if m:
            alias = m.group(1)
            continue
        m = re.match(r'#pragma\s+format\s+(\w+)', s)
        if m:
            fmt = m.group(1)
            continue
        if re.match(r'#version\b', s):
            continue
        lines.append(line)
    src = '\n'.join(lines)

    # uniform blocks -> members; instance names to rename
    members, instances = [], []
    for m in BLOCK.finditer(src):
        instances.append(m.group(4))
        for decl in strip_comments(m.group(3)).split(';'):
            decl = decl.strip()
            if not decl:
                continue
            mt = re.match(r'(?:layout\s*\([^)]*\)\s*)?(\w+)\s+(.*)', decl, re.S)
            typ = mt.group(1)
            for n in mt.group(2).split(','):
                n = n.strip()
                if n and n not in [x for x, _ in members]:
                    members.append((n, typ))
    src = BLOCK.sub('', src)
    for inst in set(instances):
        src = re.sub(r'\b%s\s*\.' % inst, 'rs.', src)

    # stages
    parts = re.split(r'#pragma\s+stage\s+(vertex|fragment)', src)
    shared, stages = parts[0], {}
    for i in range(1, len(parts), 2):
        stages[parts[i]] = stages.get(parts[i], '') + parts[i + 1]

    def samplers(text):
        names = SAMPLER.findall(text)
        return names, SAMPLER.sub('', text)

    sh_names, shared = samplers(shared)
    v_names, vert = samplers(stages.get('vertex', ''))
    f_names, frag = samplers(stages.get('fragment', ''))
    f_samplers = ['Source'] + [n for n in sh_names + f_names if n != 'Source']
    v_samplers = [n for n in sh_names + v_names if re.search(r'\b%s\b' % n, vert + shared)]

    # members ordered for std140 (big first), with offsets
    order = sorted(members, key=lambda nt: -SIZES.get(nt[1], (4, 4))[1])
    off, laid = 0, []
    for n, t in order:
        size, align = SIZES.get(t, (4, 4))
        off = (off + align - 1) // align * align
        laid.append((n, t, off))
        off += size
    ubo_size = (off + 15) // 16 * 16

    def ubo(set_):
        if not laid:
            return ''
        body = ''.join('    layout(offset = %d) %s %s;\n' % (o, t, n) for n, t, o in laid)
        return 'layout(set = %d, binding = 0, std140) uniform RogerUBO\n{\n%s} rs;\n' % (set_, body)

    # the blocks' instance names, also when reached through a macro
    # (crt-royale: #define IN params)
    head = '#version 450\n' + ''.join('#define %s rs\n' % inst for inst in sorted(set(instances)) if inst != 'rs')
    vsrc = head + ubo(1) + ''.join('layout(set = 0, binding = %d) uniform sampler2D %s;\n' % (k, n)
                                   for k, n in enumerate(v_samplers)) + shared + vert
    fsrc = head + ubo(3) + ''.join('layout(set = 2, binding = %d) uniform sampler2D %s;\n' % (k, n)
                                   for k, n in enumerate(f_samplers)) + shared + frag
    return dict(vert=vsrc, frag=fsrc, params=params, alias=alias, format=fmt,
                members=laid, ubo_size=ubo_size, fsamplers=f_samplers, vsamplers=v_samplers)


def glslang(text, stage, what):
    exe = os.environ.get('GLSLANG', 'glslang')
    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, 'shader.' + stage)
        out = os.path.join(d, 'shader.spv')
        open(src, 'w').write(text)
        r = subprocess.run([exe, '-V', '-g0', '--target-env', 'vulkan1.0', '-S', stage, '-o', out, src],
                           capture_output=True, text=True)
        if r.returncode:
            dump = os.path.join(tempfile.gettempdir(), 'roger-shader-failed.' + stage)
            open(dump, 'w').write(text)
            raise SystemExit('%s (%s): glslang failed (source in %s):\n%s' % (what, stage, dump, r.stdout + r.stderr))
        return open(out, 'rb').read()


# ---------------------------------------------------------------- semantics
FORMATS = {None: 'RS_RGBA8', 'R8G8B8A8_UNORM': 'RS_RGBA8', 'R8G8B8A8_SRGB': 'RS_SRGB8',
           'A2B10G10R10_UNORM_PACK32': 'RS_RGBA16F', 'R16G16B16A16_SFLOAT': 'RS_RGBA16F',
           'R16G16_SFLOAT': 'RS_RGBA16F', 'R16_SFLOAT': 'RS_RGBA16F',
           'R32G32B32A32_SFLOAT': 'RS_RGBA32F', 'R32G32_SFLOAT': 'RS_RGBA32F', 'R32_SFLOAT': 'RS_RGBA32F'}
WRAPS = {'clamp_to_border': 'RS_BORDER', 'clamp_to_edge': 'RS_CLAMP', 'repeat': 'RS_REPEAT',
         'mirrored_repeat': 'RS_MIRROR'}
FIXED = {'MVP': 'RS_U_MVP', 'SourceSize': 'RS_U_SOURCE_SIZE', 'OriginalSize': 'RS_U_ORIGINAL_SIZE',
         'OutputSize': 'RS_U_OUTPUT_SIZE', 'FinalViewportSize': 'RS_U_VIEWPORT_SIZE',
         'FrameCount': 'RS_U_FRAME_COUNT', 'FrameDirection': 'RS_U_FRAME_DIRECTION',
         'Rotation': 'RS_U_ZERO', 'TotalSubFrames': 'RS_U_ONE', 'CurrentSubFrame': 'RS_U_ONE',
         'OriginalAspect': 'RS_U_ASPECT', 'OriginalAspectRotated': 'RS_U_ASPECT',
         'OriginalFPS': 'RS_U_FPS', 'FrameTimeDelta': 'RS_U_FRAME_TIME'}
TYPES = {'float': 'RS_T_FLOAT', 'int': 'RS_T_INT', 'uint': 'RS_T_UINT', 'vec4': 'RS_T_VEC4',
         'mat4': 'RS_T_MAT4', 'vec2': 'RS_T_VEC2', 'vec3': 'RS_T_VEC3'}


def truthy(v):
    return str(v).lower() in ('true', '1')


def build_chain(name, preset_path):
    pre = slangp.read_preset(os.path.join(HERE, preset_path))
    pp = slangp.passes(pre)
    cfg = pre[1]
    luts = [t for t in cfg.get('textures', '').split(';') if t]
    conv = [convert(p['shader']) for p in pp]
    aliases = {}
    for i, (p, c) in enumerate(zip(pp, conv)):
        a = p.get('alias') or c['alias']
        if a:
            aliases[a] = i
    params = {}
    for c in conv:
        for k, v in c['params'].items():
            params.setdefault(k, v)
    for k in list(params):
        if k in cfg:
            params[k] = float(cfg[k])
    history = [0]
    feedback = set()

    def texref(n, i):
        """a texture name in pass i -> (kind, index)"""
        if n == 'Source':
            return ('RS_TX_ORIGINAL', 0) if i == 0 else ('RS_TX_OUTPUT', i - 1)
        if n == 'Original':
            return ('RS_TX_ORIGINAL', 0)
        m = re.fullmatch(r'OriginalHistory(\d+)', n)
        if m:
            k = int(m.group(1))
            if k == 0:
                return ('RS_TX_ORIGINAL', 0)
            history[0] = max(history[0], k)
            return ('RS_TX_HISTORY', k - 1)
        m = re.fullmatch(r'PassOutput(\d+)', n)
        if m:
            return ('RS_TX_OUTPUT', int(m.group(1)))
        m = re.fullmatch(r'PassFeedback(\d+)', n)
        if m:
            feedback.add(int(m.group(1)))
            return ('RS_TX_FEEDBACK', int(m.group(1)))
        if n in aliases:
            return ('RS_TX_OUTPUT', aliases[n])
        if n.endswith('Feedback') and n[:-8] in aliases:
            feedback.add(aliases[n[:-8]])
            return ('RS_TX_FEEDBACK', aliases[n[:-8]])
        if n in luts:
            return ('RS_TX_LUT', luts.index(n))
        return None

    def sampling(ref, i):
        """(filter, wrap, mipmap) a texture is sampled with"""
        kind, k = ref
        if kind == 'RS_TX_LUT':
            t = luts[k]
            return (truthy(cfg.get(t + '_linear', 'false')), WRAPS.get(cfg.get(t + '_wrap_mode'), 'RS_CLAMP'),
                    truthy(cfg.get(t + '_mipmap', 'false')))
        if kind in ('RS_TX_ORIGINAL', 'RS_TX_HISTORY'):
            q = pp[0]
        elif kind in ('RS_TX_OUTPUT', 'RS_TX_FEEDBACK'):
            # a pass's output is sampled as the pass after it samples its Source
            q = pp[k + 1] if k + 1 < len(pp) else pp[k]
        else:
            q = pp[i]
        return (truthy(q.get('filter_linear', 'false')), WRAPS.get(q.get('wrap_mode'), 'RS_CLAMP'),
                truthy(q.get('mipmap_input', 'false')))

    out = []
    for i, (p, c) in enumerate(zip(pp, conv)):
        what = '%s pass %d (%s)' % (name, i, os.path.relpath(p['shader'], HERE))
        vspv = glslang(c['vert'], 'vert', what)
        fspv = glslang(c['frag'], 'frag', what)
        last = i == len(pp) - 1

        def scale(axis):
            st = p.get('scale_type_' + axis, p.get('scale_type'))
            sc = p.get('scale_' + axis, p.get('scale'))
            if st is None:
                st = 'viewport' if last else 'source'
            st = {'source': 'RS_SCALE_SOURCE', 'viewport': 'RS_SCALE_VIEWPORT', 'absolute': 'RS_SCALE_ABSOLUTE'}[st]
            return st, float(sc) if sc is not None else 1.0

        sx, sy = scale('x'), scale('y')
        fmt = c['format']
        if truthy(p.get('float_framebuffer', 'false')):
            fmt = fmt or 'R16G16B16A16_SFLOAT'
        if truthy(p.get('srgb_framebuffer', 'false')):
            fmt = fmt or 'R8G8B8A8_SRGB'
        if fmt not in FORMATS:
            print('warning: %s: format %s taken as RGBA16F' % (what, fmt))
        uniforms = []
        for n, t, o in c['members']:
            if n in FIXED:
                uniforms.append((o, TYPES.get(t, 'RS_T_FLOAT'), FIXED[n], 0, 0.0))
            elif n.endswith('Size') and texref(n[:-4], i):
                kind, k = texref(n[:-4], i)
                uniforms.append((o, 'RS_T_VEC4', 'RS_U_TEX_SIZE', '%s, %d' % (kind, k), 0.0))
            elif n in params:
                uniforms.append((o, TYPES.get(t, 'RS_T_FLOAT'), 'RS_U_PARAM', 0, params[n]))
            else:
                print('warning: %s: unknown uniform %s (zero)' % (what, n))
                uniforms.append((o, TYPES.get(t, 'RS_T_FLOAT'), 'RS_U_ZERO', 0, 0.0))

        def sampler_list(names):
            res = []
            for n in names:
                ref = texref(n, i)
                if ref is None:
                    raise SystemExit('%s: unknown texture %s' % (what, n))
                f, w, mm = sampling(ref, i)
                res.append((ref, f, w, mm))
            return res
        out.append(dict(vspv=vspv, fspv=fspv, sx=sx, sy=sy, filter=truthy(p.get('filter_linear', 'false')),
                        wrap=WRAPS.get(p.get('wrap_mode'), 'RS_CLAMP'), mipmap=truthy(p.get('mipmap_input', 'false')),
                        format=FORMATS.get(fmt, 'RS_RGBA16F'), fcmod=int(p.get('frame_count_mod', '0')),
                        ubo_size=c['ubo_size'], uniforms=uniforms,
                        fs=sampler_list(c['fsamplers']), vs=sampler_list(c['vsamplers'])))
    lut_list = []
    for t in luts:
        path = cfg[t]
        lut_list.append(dict(name=t, data=open(path, 'rb').read(), linear=truthy(cfg.get(t + '_linear', 'false')),
                             mipmap=truthy(cfg.get(t + '_mipmap', 'false')),
                             wrap=WRAPS.get(cfg.get(t + '_wrap_mode'), 'RS_CLAMP')))
    return dict(name=name, passes=out, luts=lut_list, history=history[0], feedback=sorted(feedback))


# ---------------------------------------------------------------- C output
def c_bytes(data, indent='    '):
    lines = []
    for k in range(0, len(data), 20):
        lines.append(indent + ','.join('%d' % b for b in data[k:k + 20]) + ',')
    return '\n'.join(lines)


def emit(chains):
    o = ['/* generated by shaders/build.py - do not edit. The shaders come from',
         ' * shaders/own (libroger) and shaders/slang (libretro slang-shaders: see',
         ' * shaders/README and each shader\'s licence). */',
         '#include "../../src/shader_chains.h"', '']
    blobs = {}

    def blob(data):
        if data not in blobs:
            blobs[data] = 'rs_blob%d' % len(blobs)
            o.append('static const uint8_t %s[] = {\n%s\n};' % (blobs[data], c_bytes(data)))
        return blobs[data]

    for ci, ch in enumerate(chains):
        for pi, p in enumerate(ch['passes']):
            p['vname'], p['fname'] = blob(p['vspv']), blob(p['fspv'])
            o.append('static const struct rs_uniform rs_u%d_%d[] = {' % (ci, pi))
            for off, t, kind, ref, val in p['uniforms']:
                ref = ref if isinstance(ref, str) else 'RS_TX_ORIGINAL, 0'
                o.append('    {%d, %s, %s, {%s}, %ff},' % (off, t, kind, ref, val))
            o.append('    {0, 0, RS_U_END, {RS_TX_ORIGINAL, 0}, 0}};')
            for st in ('fs', 'vs'):
                o.append('static const struct rs_sampler rs_%s%d_%d[] = {' % (st, ci, pi))
                for (kind, k), f, w, mm in p[st]:
                    o.append('    {{%s, %d}, %d, %s, %d},' % (kind, k, int(f), w, int(mm)))
                o.append('    {{RS_TX_ORIGINAL, 0}, 0, RS_CLAMP, 0}};')
        for li, l in enumerate(ch['luts']):
            l['blob'] = blob(l['data'])
        o.append('static const struct rs_pass rs_passes%d[] = {' % ci)
        for pi, p in enumerate(ch['passes']):
            o.append('    {%s, sizeof %s, %s, sizeof %s, {%s, %s}, {%ff, %ff}, %d, %s, %d, %s, %d, %d,'
                     ' rs_u%d_%d, %d, rs_fs%d_%d, %d, rs_vs%d_%d},' % (
                         p['vname'], p['vname'], p['fname'], p['fname'], p['sx'][0], p['sy'][0], p['sx'][1],
                         p['sy'][1], int(p['filter']), p['wrap'], int(p['mipmap']), p['format'], p['fcmod'],
                         p['ubo_size'], ci, pi, len(p['fs']), ci, pi, len(p['vs']), ci, pi))
        o.append('};')
        if ch['luts']:
            o.append('static const struct rs_lut rs_luts%d[] = {' % ci)
            for l in ch['luts']:
                o.append('    {"%s", %s, sizeof %s, %d, %d, %s},' % (l['name'], l['blob'], l['blob'],
                                                                   int(l['linear']), int(l['mipmap']), l['wrap']))
            o.append('};')
        o.append('')
    o.append('const struct rs_chain rs_chains[] = {')
    for ci, ch in enumerate(chains):
        fb = 0
        for k in ch['feedback']:
            fb |= 1 << k
        o.append('    {"%s", %d, rs_passes%d, %d, %s, %d, 0x%xu},' % (
            ch['name'], len(ch['passes']), ci, len(ch['luts']), 'rs_luts%d' % ci if ch['luts'] else 'NULL',
            ch['history'], fb))
    o.append('};')
    o.append('const int rs_nchains = %d;' % len(chains))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, 'w').write('\n'.join(o) + '\n')


def main():
    if len(sys.argv) > 2 and sys.argv[1] == 'vendor':
        vendor(sys.argv[2])
        return
    only = sys.argv[1:]
    chains = []
    for name, preset in CHAINS:
        if only and name not in only:
            continue
        print('building', name, flush=True)
        chains.append(build_chain(name, preset))
    emit(chains)
    print('wrote', OUT, '(%d KB)' % (os.path.getsize(OUT) // 1024))


if __name__ == '__main__':
    main()
