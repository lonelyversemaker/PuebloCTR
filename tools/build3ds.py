#!/usr/bin/env python3
"""PuebloCTR: the game for the New Nintendo 3DS (PuebloCTR.3dsx; the .cia: tools/build_cia.sh) from the build tree.

The tree's Linux build (port/build/whole-armhf-O2-low-static, made by tools/build.sh: prepared/ - the game's sources
prepared, generated/ - stubs, module imports) gives the compile commands; each is turned into one for the 3DS
(clang --target=armv6k-none-eabihf, the headers of devkitARM's newlib and libstdc++) and compiled, with PuebloCTR's 3DS
layer (port/whole/3ds). Then:
  - libre4_main (the game, its hooks, glue, CRI, card, stubs) - one object (ld -r), the game's operator new/delete
    renamed (re4g_*): the platform and libstdc++ keep the system's;
  - the REL modules linked into the image (port/tools/whole_static_modules.py with devkitARM's binutils);
  - the link (arm-none-eabi-g++ -specs=3dsx.specs, libctru, citro3d), smdhtool, 3dsxtool.
A second run compiles only what changed (.d files).

    python3 tools/build3ds.py --tree TREE [--release] [--version TEXT] [--jobs N] [--out DIR]

--release: the release build (no log unless asked, no L menu; PuebloCTR.3dsx); without: the development build
(re4.3dsx, "PuebloCTR dev"). Runs in the build image (docker/Dockerfile): devkitARM r68, libctru 2.7.0, citro3d 1.7.1,
LLVM 22.
"""
import argparse
import json
import os
import re
import shlex
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

DKP = os.environ.get("DEVKITPRO", "/opt/devkitpro")
ARM = DKP + "/devkitARM"
BIN = ARM + "/bin/arm-none-eabi-"
TOOLS = DKP + "/tools/bin/"
GCCVER = "16.1.0"  # devkitARM r68's
CLANG = os.environ.get("PUEBLO_CLANG", "clang")
ARCH = ["-march=armv6k", "-mtune=mpcore", "-mfloat-abi=hard", "-mfpu=vfp", "-mtp=soft"]
DEFS = ["-D__3DS__", "-DRE4_3DS_PROBE"]
CTRU = ["-isystem", DKP + "/libctru/include"]
CLANG_INC = [
    "-nostdinc", "-nostdinc++",
    "-isystem", ARM + "/arm-none-eabi/include/c++/" + GCCVER,
    "-isystem", ARM + "/arm-none-eabi/include/c++/" + GCCVER + "/arm-none-eabi/armv6k/fpu",
    "-isystem", ARM + "/arm-none-eabi/include/c++/" + GCCVER + "/backward",
    "-isystem", ARM + "/lib/gcc/arm-none-eabi/" + GCCVER + "/include",
    "-isystem", ARM + "/lib/gcc/arm-none-eabi/" + GCCVER + "/include-fixed",
    "-isystem", ARM + "/arm-none-eabi/include",
]
LINUX_BUILD = "whole-armhf-O2-low-static"
MAIN_TARGETS = ["re4_main_obj", "re4_gamehooks_obj", "re4_glue_obj", "re4_cri_obj", "re4_card_obj"]
PLATFORM_TARGETS = ["re4_platform_obj", "re4_formats_obj", "re4_sdk_obj", "re4_reuse_obj"]
GAME_NEW = ["_Znwj", "_Znaj", "_ZdlPv", "_ZdaPv"]
# PGO, the build by profile (2026-11-21): pgo/re4.profdata of the repository (how often each function of the game ran
# in replays on Linux; pgo/README.md), applied to these groups' files: mod (the game's modules), main (the game's main
# part); also possible: platform (runtime, SDK), 3ds. PUEBLO_PGO=off: without; PUEBLO_PGO=FILE: another profile.
_PGO_DEFAULT = Path(__file__).resolve().parents[1] / "pgo" / "re4.profdata"
PGO = os.environ.get("PUEBLO_PGO", str(_PGO_DEFAULT) if _PGO_DEFAULT.is_file() else "")
PGO = "" if PGO == "off" else PGO
PGO_GROUPS = os.environ.get("PUEBLO_PGO_GROUPS", "mod,main").split(",")
# (experiments) more flags for every clang compile, e.g. PUEBLO_CLANG_EXTRA="-munaligned-access"; a PGO group may
# also be a target's name (re4_sdk_obj: the SDK alone out of the platform group)
CLANG_EXTRA = os.environ.get("PUEBLO_CLANG_EXTRA", "").split()
# for the New 3DS CPU, every clang compile (2026-11-22, measured on the console against the same tree): word loads and
# stores of unaligned data (the ARM11 does them; clang's default for armv6k builds them byte by byte, gcc's does not)
# and no errno from math functions (one compare less after each square root; values the same) - fight -0.4 ms, the
# village path -1.2 ms. PUEBLO_CPU_FLAGS=off: without
CPU_FLAGS = [] if os.environ.get("PUEBLO_CPU_FLAGS") == "off" else ["-munaligned-access", "-fno-math-errno"]

TREE = OUT = LBUILD = GEN = None
RELEASE = False


def translate(entry):
    """A command of the Linux build -> the 3DS's (clang, ARMv6K + VFPv2, devkitARM's headers), -fsyntax-only + source."""
    args = shlex.split(entry["command"])
    src = entry["file"]
    out = []
    i = 1
    while i < len(args):
        a = args[i]
        if a in ("-o", "-MF", "-MT", "-c"):
            i += 2
            continue
        if a in ("-MD", "-fPIC", "-fpic") or a.startswith("--target=") or a.startswith("-ferror-limit"):
            i += 1
            continue
        if a in ("-include", "-isystem", "-I"):
            out += [a, args[i + 1]]
            i += 2
            continue
        if not a.startswith("-fmacro-prefix-map="):
            out.append(a)
        i += 1
    if not src.endswith(".c") and not any(a.startswith("-std=") for a in out):
        out.append("-std=gnu++17")  # (CMake leaves it out where the compiler defaults to C++17)
    return [CLANG, "--target=armv6k-none-eabihf"] + ARCH[:3] + ["-mfpu=vfp2"] + CLANG_INC + DEFS + CTRU + out + \
        ["-w", "-fsyntax-only", src]


def target_of(entry):
    m = re.search(r"CMakeFiles/([^/]+)\.dir/", entry["output"])
    return m.group(1) if m else None


def obj_path(target, src):
    rel = os.path.relpath(src, TREE)
    rel = rel.replace("port/build/" + LINUX_BUILD + "/", "lbuild/")
    return OUT / "obj" / target / (rel + ".o")


def command(entry, out):
    cmd = translate(entry)
    src = cmd.pop()
    cmd.remove("-fsyntax-only")
    if "-w" in cmd:
        cmd.remove("-w")
    i = cmd.index("--target=armv6k-none-eabihf") + 1
    cmd[i:i] = ["-mtp=soft", "-isystem", str(TREE / "port/whole/3ds/include"), "-isystem", str(GEN)]
    return cmd + ["-w", "-c", "-MMD", "-MF", str(out) + ".d", "-o", str(out), src]


def run(cmd, **kw):
    p = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kw)
    if p.returncode != 0:
        sys.exit(f"{cmd[0]} failed:\n{p.stdout[-4000:]}{p.stderr[-6000:]}")
    return p.stdout


def shaders():
    """port/whole/3ds/*.v.pica -> picasso -> GEN/<name>_shbin.h (unsigned char <name>_shbin[], <name>_shbin_size)."""
    GEN.mkdir(parents=True, exist_ok=True)
    for src in sorted((TREE / "port/whole/3ds").glob("*.v.pica")):
        name = src.name[:-len(".v.pica")]
        shbin = GEN / (name + ".shbin")
        header = GEN / (name + "_shbin.h")
        if header.exists() and header.stat().st_mtime >= src.stat().st_mtime:
            continue
        run([TOOLS + "picasso", "-o", shbin, src])
        data = shbin.read_bytes()
        rows = ",\n".join(", ".join(str(b) for b in data[i:i + 24]) for i in range(0, len(data), 24))
        header.write_text(f"// picasso {src.name} (build3ds.py)\n#pragma once\n"
                          f"alignas(4) static const unsigned char {name}_shbin[] = {{\n{rows}\n}};\n"
                          f"static const unsigned {name}_shbin_size = {len(data)};\n")


def version(text):
    """GEN/re4_version.h: RE4_3DS_VERSION (the bottom screen, crash.txt); written only when it changes."""
    h = GEN / "re4_version.h"
    body = f'#pragma once\n#define RE4_3DS_VERSION "{text}"\n'
    GEN.mkdir(parents=True, exist_ok=True)
    if not h.exists() or h.read_text() != body:
        h.write_text(body)
    print(f"version: {text}")


def up_to_date(out):
    dep = Path(str(out) + ".d")
    if not out.exists() or not dep.exists():
        return False
    t = out.stat().st_mtime
    text = dep.read_text().replace("\\\n", " ")
    files = shlex.split(text.split(":", 1)[1]) if ":" in text else []
    return all(Path(f).exists() and Path(f).stat().st_mtime <= t for f in files)


def compile_all(jobs_list, jobs):
    todo = [(c, o) for c, o in jobs_list if not up_to_date(o)]
    print(f"compile: {len(todo)} of {len(jobs_list)} files", flush=True)
    errors = []

    def one(item):
        c, o = item
        o.parent.mkdir(parents=True, exist_ok=True)
        p = subprocess.run(c, capture_output=True, text=True)
        return o, p.returncode, p.stderr

    done = 0
    with ThreadPoolExecutor(jobs) as ex:
        for o, rc, err in ex.map(one, todo):
            done += 1
            if rc != 0:
                errors.append((o, err))
            if done % 200 == 0:
                print(f"  {done}/{len(todo)}, errors {len(errors)}", flush=True)
    log = OUT / "compile-errors.txt"
    log.write_text("".join(f"== {o}\n{e}\n" for o, e in errors))
    if errors:
        for o, e in errors[:15]:
            first = next((l for l in e.splitlines() if "error" in l), e[:200])
            print(f"  FAILED {o.name}: {first}")
        sys.exit(f"compile: {len(errors)} failed (all: {log})")


def linker_script(fragment):
    """3dsx.ld with the static modules' sections (whole_static_modules.py --ldscript) inside the data segment: data and
    constructors before __bss_start__, bss inside .bss (3dsxtool and the loader map the data segment only as far as
    .bss goes). Returns the specs file that links with it."""
    lib = Path(ARM) / "arm-none-eabi/lib"
    lines = fragment.read_text().splitlines()
    data = ["\t" + l.strip() + "\n" for l in lines if l.strip().startswith(("re4d_", "re4i_"))]
    bss = []
    for l in lines:
        m = re.match(r"\s*(re4b_\w+) \(NOLOAD\)", l)
        if m:
            n = m.group(1)
            bss.append(f"\t\t. = ALIGN(8);\n\t\t__start_{n} = .;\n\t\t*({n})\n\t\t__stop_{n} = .;\n")
    ld = (lib / "3dsx.ld").read_text()
    for anchor, add in (("\t__bss_start__ = .;", data), ("\t\t*(COMMON)\n", bss)):
        if ld.count(anchor) != 1:
            sys.exit(f"3dsx.ld: {anchor.strip()!r} expected once")
        ld = ld.replace(anchor, "".join(add) + anchor if anchor.startswith("\t__") else anchor + "".join(add))
    script = OUT / "3dsx-re4.ld"
    script.write_text(ld)
    specs = (lib / "3dsx.specs").read_text()
    if specs.count("-T 3dsx.ld%s") != 1:
        sys.exit("3dsx.specs: '-T 3dsx.ld%s' expected once")
    path = OUT / "3dsx-re4.specs"
    path.write_text(specs.replace("-T 3dsx.ld%s", f"-T {script}"))
    return path


def main():
    global TREE, OUT, LBUILD, GEN, RELEASE
    ap = argparse.ArgumentParser()
    ap.add_argument("--tree", required=True)
    ap.add_argument("--release", action="store_true")
    ap.add_argument("--version", default=os.environ.get("PUEBLO_VERSION", "dev"))
    ap.add_argument("--jobs", type=int, default=os.cpu_count())
    ap.add_argument("--out")
    a = ap.parse_args()
    TREE = Path(a.tree).resolve()
    RELEASE = a.release
    OUT = Path(a.out).resolve() if a.out else TREE / "port/build" / ("3ds-release" if RELEASE else "3ds-dev")
    LBUILD = TREE / "port/build" / LINUX_BUILD
    GEN = OUT / "gen"
    OUT.mkdir(parents=True, exist_ok=True)
    cc_json = OUT / "compile_commands.json"
    cc_json.write_text(run(["ninja", "-C", LBUILD, "-t", "compdb"]))
    entries = json.loads(cc_json.read_text())

    groups, jobs_list, by_target = {}, [], {}
    for e in entries:
        t = target_of(e)
        if t:
            by_target.setdefault(t, []).append(e)
    flags_platform = next(e for e in by_target["re4_platform_obj"] if e["file"].endswith("runtime/module_loader.cpp"))
    static_cpp_entry = next(e for e in by_target["re4_main"] if e["file"].endswith("static_modules.cpp"))
    stubs_entry = next(e for e in by_target["re4_main"] if e["file"].endswith("stubs.cpp"))
    modules_entry = next(e for e in by_target["re4_platform"] if e["file"].endswith("modules.cpp"))

    def add(group, target, entry, src=None):
        if src:
            entry = dict(entry, command=entry["command"].replace(entry["file"], src), file=src)
        o = obj_path(target, entry["file"])
        c = command(entry, o)
        if RELEASE and group == "3ds":
            c = c[:1] + ["-DRE4_3DS_RELEASE=1"] + c[1:]
        if PGO and (group.split(":")[0] in PGO_GROUPS or target in PGO_GROUPS):
            c = c[:1] + ["-fprofile-instr-use=" + PGO] + c[1:]
        if CPU_FLAGS + CLANG_EXTRA:
            c = c[:1] + CPU_FLAGS + CLANG_EXTRA + c[1:]
        jobs_list.append((c, o))
        groups.setdefault(group, []).append(o)

    for t in MAIN_TARGETS:
        for e in by_target[t]:
            add("main", t, e)
    add("main", "re4_main", stubs_entry)
    for t in PLATFORM_TARGETS:
        for e in by_target[t]:
            add("platform", t, e)
    add("platform", "re4_platform", modules_entry)
    modules = sorted(t[len("re4_mod_"):-len("_obj")] for t in by_target if t.startswith("re4_mod_") and t.endswith("_obj"))
    for m in modules:
        for t in (f"re4_mod_{m}_obj", f"re4_modcrt_{m}_obj"):
            for e in by_target[t]:
                add("mod:" + m, t, e)
    shaders()
    version(a.version)
    for src in sorted((TREE / "port/whole/3ds").glob("*.cpp")):
        add("3ds", "re4_3ds", flags_platform, str(TREE / "port/whole/3ds" / src.name))
    print(f"PGO: {PGO} ({','.join(PGO_GROUPS)})" if PGO else "PGO: off")
    compile_all(jobs_list, a.jobs)

    main_o = OUT / "libre4_main.o"
    run([BIN + "ld", "-r", "-o", main_o, *groups["main"]])
    run([BIN + "objcopy", *[f"--redefine-sym={s}=re4g_{s}" for s in GAME_NEW], main_o])

    objects_txt = OUT / "static_objects.txt"
    objects_txt.write_text("".join(f"{m}=" + ";".join(map(str, groups["mod:" + m])) + "\n" for m in modules))
    static_dir = OUT / "static"
    wrap = sum((["--wrap", w] for w in GAME_NEW), [])
    print(run([sys.executable, TREE / "port/tools/whole_static_modules.py", "--objects", objects_txt,
               "--imports", LBUILD / "generated/imports", "--out", static_dir, "--ld", BIN + "ld",
               "--objcopy", BIN + "objcopy", "--nm", BIN + "nm", "--readelf", BIN + "readelf",
               *wrap, "--ldscript", static_dir / "static_modules.ld"]).strip())
    table_src = str(static_dir / "static_modules.cpp")
    table_o = obj_path("re4_main", table_src)
    table_entry = dict(static_cpp_entry, command=static_cpp_entry["command"].replace(static_cpp_entry["file"], table_src),
                       file=table_src)
    compile_all([(command(table_entry, table_o), table_o)], 1)

    elf = OUT / "re4.elf"
    static_objs = [static_dir / f"{m}.o" for m in modules]
    specs = linker_script(static_dir / "static_modules.ld")
    # the game's and the port's enums stay 32-bit (the Linux build's layout of the game's structures)
    run([BIN + "g++", "-specs=" + str(specs), *ARCH, "-Wl,-Map," + str(OUT / "re4.map"),
         "-Wl,--no-enum-size-warning",
         "-Wl,--wrap=abort,--wrap=exit",  # profile_3ds.cpp: where an abort/exit came from
         # gx_3ds.cpp (finding 4, 2026-11-24): the VRAM free total kept until anyone's next alloc / free - where
         # the tree has the wrappers (an older tree links without them)
         *(["-Wl,--wrap=vramAlloc,--wrap=vramAllocAt,--wrap=vramMemAlign,--wrap=vramMemAlignAt,--wrap=vramFree"]
           if "__wrap_vramAlloc" in (TREE / "port/whole/3ds/gx_3ds.cpp").read_text() else []),
         *(["-Wl,--wrap=linearAlloc,--wrap=linearMemAlign,--wrap=linearFree,--wrap=linearSpaceFree,--wrap=vramSpaceFree"]  # gx_3ds.cpp: the allocators' lock (the GX split, 2026-11-26)
           if "__wrap_linearAlloc" in (TREE / "port/whole/3ds/gx_3ds.cpp").read_text() else []),
         "-Wl,--wrap=C3D_UpdateUniforms",  # gx_3ds.cpp: only when a uniform of the type is dirty
         "-o", elf, main_o, *groups["platform"], *groups["3ds"], table_o, *static_objs,
         "-L" + DKP + "/libctru/lib", "-lpthread", "-lcitro3d", "-lctru", "-lm"])
    print(run([BIN + "size", elf]).strip())
    smdh = OUT / "re4.smdh"
    title = ("PuebloCTR", "Unofficial New 3DS port, " + a.version, "PuebloCTR") if RELEASE else \
        ("PuebloCTR dev", "Development build (logs, the L menu, the speed panel)", "PuebloCTR")
    run([TOOLS + "smdhtool", "--create", *title, TREE / "port/whole/3ds/cia/icon.png", smdh])
    out = OUT / ("PuebloCTR.3dsx" if RELEASE else "PuebloCTR-dev.3dsx")
    run([TOOLS + "3dsxtool", elf, out, "--smdh=" + str(smdh)])
    print(f"ok: {out} ({out.stat().st_size >> 10} KB)")


if __name__ == "__main__":
    main()
