"""Reading RetroArch shader presets (.slangp) and slang shaders: the preset
format and the #include / #pragma conventions of libretro's slang-shaders."""
import os
import re


def read_preset(path):
    """key -> value of a .slangp file, with #reference'd presets merged
    (the referencing preset's own keys win)."""
    vals = {}
    refs = []
    for line in open(path, encoding='utf-8', errors='replace'):
        line = line.strip()
        if line.startswith('#reference'):
            refs.append(os.path.normpath(os.path.join(os.path.dirname(path), line.split(None, 1)[1].strip().strip('"'))))
            continue
        line = re.sub(r'(^|\s)(#|//).*$', '', line).strip()
        if not line or '=' not in line:
            continue
        k, v = line.split('=', 1)
        vals[k.strip()] = v.strip().strip('"').strip()
    merged = {}
    for r in refs:
        base, sub = read_preset(r)
        merged.update(sub)
    # paths in the preset are relative to it
    out = dict(merged)
    out.update({k: v for k, v in vals.items()})
    for k, v in vals.items():
        if re.fullmatch(r'shader\d+', k) or k in (vals.get('textures', '').split(';')):
            out[k] = os.path.normpath(os.path.join(os.path.dirname(path), v))
    return path, out


def passes(preset):
    """the preset's passes: dicts of the pass's keys without the index"""
    _, p = preset
    n = int(p['shaders'])
    res = []
    for i in range(n):
        d = {}
        for k, v in p.items():
            m = re.fullmatch(r'([a-z_]+?)(\d+)', k)
            if m and int(m.group(2)) == i:
                d[m.group(1)] = v
        res.append(d)
    return res


def expand_includes(path, seen=None):
    """the shader source with #include "file" expanded (relative paths)"""
    out = []
    for line in open(path, encoding='utf-8', errors='replace'):
        m = re.match(r'\s*#include\s+"([^"]+)"', line)
        if m:
            inc = os.path.normpath(os.path.join(os.path.dirname(path), m.group(1)))
            out.append(expand_includes(inc))
        else:
            out.append(line)
    return ''.join(out)


def includes(path, acc=None):
    """every file a shader includes, recursively (itself first)"""
    acc = acc if acc is not None else []
    if path in acc:
        return acc
    acc.append(path)
    for line in open(path, encoding='utf-8', errors='replace'):
        m = re.match(r'\s*#include\s+"([^"]+)"', line)
        if m:
            includes(os.path.normpath(os.path.join(os.path.dirname(path), m.group(1))), acc)
    return acc
