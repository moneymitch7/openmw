#!/usr/bin/env python3
"""Compile-check OpenMW shaders the way the engine builds them.

Emulates components/shader/shadermanager.cpp: #include expansion, @define
substitution (undefined names are errors, like the engine), @foreach and
@link, then compiles every program (main shader plus linked units) with
glslangValidator for a set of setting combinations.

usage: shadercheck.py <openmw source root> [--verbose]
"""
import itertools
import os
import re
import subprocess
import sys
import tempfile

SEP = " \n\r()[].;,"


def find_first_of(s, chars, start=0):
    for i in range(start, len(s)):
        if s[i] in chars:
            return i
    return -1


def root_prefix(p):
    for r in ("lib", "compatibility", "core"):
        if p.startswith(r):
            return r
    return ""


def parse_includes(root, source, name, seen):
    if name in seen:
        raise RuntimeError(f"{name}: cyclic include")
    seen = seen | {name}
    source = source.replace("\r\n", "\n")
    while True:
        pos = source.find("#include")
        if pos < 0:
            return source
        start = source.find('"', pos)
        end = source.find('"', start + 1)
        inc = source[start + 1:end]
        if not root_prefix(inc):
            inc = os.path.normpath(os.path.join(os.path.dirname(name), inc))
        path = os.path.join(root, inc)
        if not os.path.exists(path):
            raise RuntimeError(f"{name}: failed to open include {inc}")
        with open(path, encoding="utf-8") as f:
            body = parse_includes(root, f.read(), inc, seen)
        source = source[:pos] + "\n" + body + "\n" + source[end + 1:]


def parse_defines(source, defines, name):
    iters = []
    while True:
        pos = source.find("@")
        if pos < 0:
            return source
        end = find_first_of(source, SEP, pos)
        if end < 0:
            raise RuntimeError(f"{name}: unexpected EOF")
        d = source[pos + 1:end]
        if d == "foreach":
            source = source[:pos] + "$" + source[pos + 1:]
            s = end + 1
            e = find_first_of(source, SEP, s)
            iters.append(source[s:e])
        elif d == "endforeach":
            source = source[:pos] + "$" + source[pos + 1:]
            if not iters:
                raise RuntimeError(f"{name}: endforeach without foreach")
            iters.pop()
        elif d == "link" or d in iters:
            source = source[:pos] + "$" + source[pos + 1:]
        elif d in defines:
            source = source[:pos] + str(defines[d]) + source[end:]
        else:
            raise RuntimeError(f"{name}: undefined @{d}")


LINK_RE = re.compile(r'\s*"([^"]+)"\s*(if\s+(!)?\s*([_a-zA-Z0-9]+)?)?\s*')


def parse_directives(source, name):
    links = []
    pos = 0
    while True:
        pos = source.find("$", pos)
        if pos < 0:
            return source, links
        end = find_first_of(source, SEP, pos)
        d = source[pos + 1:end]
        if d == "foreach":
            s = pos + len("$foreach") + 1
            e = find_first_of(source, SEP, s)
            it = "$" + source[s:e]
            le = find_first_of(source, "\n\r", e + 1)
            lst = source[e + 1:le]
            elems = lst.split(",") if lst else []
            cs = le
            while source[cs] in "\n\r":
                cs += 1
            ce = source.find("$endforeach", cs)
            content = source[cs:ce]
            rep = "".join(content.replace(it, el) for el in elems)
            source = source[:pos] + rep + "\n" + source[ce + len("$endforeach"):]
        elif d == "link":
            le = source.find("\n", pos + 5)
            stmt = source[pos + 5:le]
            m = LINK_RE.fullmatch(stmt)
            if not m:
                raise RuntimeError(f"{name}: bad $link {stmt!r}")
            target = m.group(1)
            if m.group(2):
                cond = not (m.group(4) in (None, "", "0"))
                if m.group(3) == "!":
                    cond = not cond
                if not cond:
                    target = ""
            if target and target != name:
                links.append(target)
            source = source[:pos] + source[le:]
        else:
            raise RuntimeError(f"{name}: unknown directive ${d}")


def build(root, name, defines, cache):
    key = name
    if key in cache:
        return cache[key]
    with open(os.path.join(root, name), encoding="utf-8") as f:
        src = parse_includes(root, f.read(), name, set())
    src = parse_defines(src, defines, name)
    src, links = parse_directives(src, name)
    cache[key] = (src, links)
    return cache[key]


def program(root, name, defines):
    """Main unit plus all units it links (recursively), as (name, source)."""
    cache, out, todo = {}, [], [name]
    while todo:
        n = todo.pop(0)
        if any(n == o[0] for o in out):
            continue
        src, links = build(root, n, defines, cache)
        out.append((n, src))
        todo.extend(links)
    return out


# Engine-wide defines (renderingmanager, lightmanager, shadow.cpp) and the
# per-material defines ShaderVisitor sets. Values are what a typical scene
# produces; the matrix below flips the ones that matter.
BASE = {
    # global (renderingmanager)
    "forcePPL": 1, "clamp": 1, "preLightEnv": 0, "classicFalloff": 0,
    "radialFog": 0, "exponentialFog": 0, "skyBlending": 0,
    "particlePointLighting": 0, "reverseZ": 1, "lampShadows": 0, "mgeFog": 0,
    "groundcoverFadeStart": "0.9", "groundcoverFadeEnd": "1.0",
    "groundcoverStompMode": 2, "groundcoverStompIntensity": 1,
    "useOVR_multiview": 0, "numViews": 1, "disableNormals": 0,
    "distorionRTRatio": "0.5",
    # lightmanager
    "maxLights": 8, "maxLightsPerCluster": 64, "lightingMethodClustered": 1,
    "useGPUShader4": 1, "simpleLighting": 0,
    # shadows
    "shadows_enabled": 1, "shadow_texture_unit_list": "0,1,2",
    "useShadowDebugOverlay": 0, "perspectiveShadowMaps": 1,
    "disableNormalOffsetShadows": 0, "shadowNormalOffset": "0.0",
    "limitShadowMapDistance": 1,
    # material (ShaderVisitor)
    # ShaderVisitor sets each map's UV define to its texture unit, so they differ
    "diffuseMap": 1, "diffuseMapUV": 0, "normalMap": 1, "normalMapUV": 1,
    "specularMap": 1, "specularMapUV": 2, "emissiveMap": 1, "emissiveMapUV": 3,
    "envMap": 1, "envMapUV": 4, "glossMap": 1, "glossMapUV": 5,
    "detailMap": 1, "detailMapUV": 6, "decalMap": 1, "decalMapUV": 7,
    "darkMap": 0, "darkMapUV": 1, "bumpMap": 0, "bumpMapUV": 2,  # see "dark+bump"
    "blendMap": 1, "parallax": 1, "diffuseParallax": 0, "alphaFunc": 0,
    "alphaToCoverage": 0, "adjustCoverage": 0, "additiveBlending": 0,
    "particle": 0, "particleOcclusion": 0, "softParticles": 0,
    "reconstructNormalZ": 0,
    # water
    "waterRefraction": 1, "rippleMapSize": "1024.0",
    "rippleMapWorldScale": "2.0", "rainRippleDetail": 1,
    "wobblyShores": 1, "sunlightScattering": 1,
    # only on the openmge-xe fork (unused defines are harmless upstream)
    "diffuseMapArray": 0, "lightCap": 1, "mgeWxWater": 0, "scaledNormalOffset": 1,
    "shadowMapResolution": "2048.0", "softShadows": 0, "waveGridHalfSize": 64, "waves": 0,
    # misc
    "hdrExposureTime": "0.05", "workGroupSize": 16,
}

# (name, overrides) – each program is compiled under each of these.
MATRIX = []
for clustered, shadows, lamp, simple in itertools.product((1, 0), (1, 0), (1, 0), (0, 1)):
    if simple and clustered:
        continue  # simple lighting is a shader-light path, not clustered
    MATRIX.append((
        f"clustered={clustered} shadows={shadows} lampShadows={lamp} simple={simple}",
        {"lightingMethodClustered": clustered, "shadows_enabled": shadows,
         "lampShadows": lamp, "simpleLighting": simple,
         "shadow_texture_unit_list": "0,1,2" if shadows else ""}))
# material extremes on the default lighting path
MATRIX.append(("bare material", {k: 0 for k in (
    "diffuseMap", "normalMap", "specularMap", "emissiveMap", "envMap",
    "glossMap", "detailMap", "decalMap", "darkMap", "bumpMap", "parallax")}))
MATRIX.append(("dark+bump", {"darkMap": 1, "bumpMap": 1, "normalMap": 0, "specularMap": 0, "parallax": 0}))  # parallax needs a normal map
MATRIX.append(("fork extras", {"waves": 1, "softShadows": 1, "mgeWxWater": 1, "diffuseMapArray": 1}))
# [Fog] mge fog, with the fog settings it meets
for sky, radial, clustered in itertools.product((1, 0), (1, 0), (1, 0)):
    MATRIX.append((f"mgeFog skyBlending={sky} radialFog={radial} clustered={clustered}",
                   {"mgeFog": 1, "skyBlending": sky, "radialFog": radial, "exponentialFog": radial,
                    "lightingMethodClustered": clustered}))
MATRIX.append(("mgeFog particles", {"mgeFog": 1, "skyBlending": 1, "particle": 1, "softParticles": 1,
                                    "alphaFunc": 1, "additiveBlending": 1}))
MATRIX.append(("particles", {"particle": 1, "softParticles": 1, "particleOcclusion": 1,
                             "alphaFunc": 1, "additiveBlending": 1}))

PROGRAMS = [
    ("compatibility/objects.vert", "vert"), ("compatibility/objects.frag", "frag"),
    ("compatibility/terrain.vert", "vert"), ("compatibility/terrain.frag", "frag"),
    ("compatibility/groundcover.vert", "vert"), ("compatibility/groundcover.frag", "frag"),
    ("compatibility/water.vert", "vert"), ("compatibility/water.frag", "frag"),
    ("compatibility/sky.vert", "vert"), ("compatibility/sky.frag", "frag"),
    ("compatibility/shadowcasting.vert", "vert"), ("compatibility/shadowcasting.frag", "frag"),
]


def compile_program(units, stage, tmp):
    files = []
    for i, (n, src) in enumerate(units):
        if "GL_EXT_gpu_shader4" in src:
            # glslang has no EXT_gpu_shader4; GLSL 1.30 core has the same
            # features (integers, bit ops, textureSize), so emulate it.
            src = re.sub(r"#extension\s+GL_EXT_gpu_shader4\s*:\s*require", "", src)
            src = src.replace("#version 120", "#version 130\n#define textureSize2D textureSize", 1)
        p = os.path.join(tmp, f"{i}_{os.path.basename(n).replace('.', '_')}.{stage}")
        with open(p, "w") as f:
            f.write(src)
        files.append(p)
    r = subprocess.run(["glslangValidator", "-l", *files], capture_output=True, text=True)
    return r.returncode, (r.stdout + r.stderr).strip(), files


def glsl_preprocessed(units, stage, tmp, tag):
    """Each unit after glslang's own preprocessor, blank lines and #line markers dropped."""
    out = []
    for i, (n, src) in enumerate(units):
        if "GL_EXT_gpu_shader4" in src:
            src = re.sub(r"#extension\s+GL_EXT_gpu_shader4\s*:\s*require", "", src)
            src = src.replace("#version 120", "#version 130\n#define textureSize2D textureSize", 1)
        p = os.path.join(tmp, f"{tag}{i}.{stage}")
        with open(p, "w") as f:
            f.write(src)
        r = subprocess.run(["glslangValidator", "-E", p], capture_output=True, text=True)
        lines = [l.strip() for l in r.stdout.splitlines()]
        out.append((n, [l for l in lines if l and not l.startswith("#line")]))
    return out


def same_when_off(root, base_root):
    """Programs built with mgeFog off must come out as the baseline tree's."""
    diffs = 0
    total = 0
    with tempfile.TemporaryDirectory() as tmp:
        for name, stage in PROGRAMS:
            for label, over in MATRIX:
                d = dict(BASE, **over)
                if d.get("mgeFog"):
                    continue
                total += 1
                a = glsl_preprocessed(program(root, name, d), stage, tmp, "a")
                b = glsl_preprocessed(program(base_root, name, d), stage, tmp, "b")
                if a != b:
                    diffs += 1
                    print(f"DIFF  {name} [{label}]")
    print(f"\n{total - diffs}/{total} programs identical to the baseline with mgeFog off")
    return 1 if diffs else 0


def main():
    root = os.path.join(sys.argv[1], "files", "shaders")
    verbose = "--verbose" in sys.argv
    if "--same-when-off" in sys.argv:
        base = sys.argv[sys.argv.index("--same-when-off") + 1]
        return same_when_off(root, os.path.join(base, "files", "shaders"))
    failures = 0
    total = 0
    seen_errors = {}
    with tempfile.TemporaryDirectory() as tmp:
        for name, stage in PROGRAMS:
            for label, over in MATRIX:
                total += 1
                d = dict(BASE, **over)
                try:
                    units = program(root, name, d)
                except RuntimeError as e:
                    failures += 1
                    print(f"FAIL  {name} [{label}] preprocess: {e}")
                    continue
                rc, out, files = compile_program(units, stage, tmp)
                if rc != 0:
                    failures += 1
                    msg = "\n".join(l for l in out.splitlines() if "ERROR" in l)[:2000]
                    key = (name, msg)
                    if key in seen_errors and not verbose:
                        print(f"FAIL  {name} [{label}] (same errors as [{seen_errors[key]}])")
                    else:
                        seen_errors[key] = label
                        print(f"FAIL  {name} [{label}]\n{msg}")
                elif verbose:
                    print(f"ok    {name} [{label}] ({len(units)} units)")
    print(f"\n{total - failures}/{total} programs compiled")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
