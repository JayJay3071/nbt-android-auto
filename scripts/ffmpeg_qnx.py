"""FFmpeg 2.8.22 (H.264 decoder only) for our decoder program nbt_decoder, built in the QNX 6.5.0 VM.

The VM has no bash, perl or yasm, so FFmpeg's configure cannot run there and its assembly cannot be assembled there.
Instead:
  - FFmpeg's configure ran once on Linux (gcc -m32, yasm, same options as below); its config.h, config.asm,
    libavutil/avconfig.h and libavutil/ffversion.h were adjusted for QNX 6.5 / GCC 4.4.2 by sanitize() and are kept in
    src/android_auto/decoder/ffmpeg/. Values that depend on the QNX C library are probed in the VM by
    scripts/build-ffmpeg-qnx.sh before compiling.
  - src/android_auto/decoder/ffmpeg/sources.txt lists every FFmpeg file the build needs with its SHA-256,
    objects.txt the .c and .asm files that go into libavcodec.a and libavutil.a (the same objects
    "make libavcodec/libavcodec.a libavutil/libavutil.a" builds for this configuration).
  - build_all.py takes the files from an FFmpeg checkout at tag n2.8.22 (cloned into .local/ once), assembles the .asm
    files here with yasm 1.3.0 (the yasm on PATH, or on Windows the official yasm-1.3.0-win64.exe, downloaded once into
    .local/, SHA-256 checked) into ELF objects, and the VM compiles the .c files with qcc and puts everything into
    libavcodec.a and libavutil.a (scripts/build-ffmpeg-qnx.sh).

FFmpeg is LGPL 2.1 or later; with these options no GPL or non-free parts are enabled.

Regenerate (Linux with gcc-multilib and yasm, only needed when the FFmpeg version or options change):
  mkdir cfg && cd cfg && <ffmpeg>/configure <CONFIGURE_ARGS> --cc="gcc -m32 -fno-pic -no-pie" --yasmexe=yasm
  python3 scripts/ffmpeg_qnx.py generate <ffmpeg checkout> cfg
"""
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FFMPEG_TAG = "n2.8.22"
FFMPEG_COMMIT = "25b3a5ef00605b37f34d49007f5c463bc6882687"
FFMPEG_GIT = "https://github.com/FFmpeg/FFmpeg.git"
YASM_URL = "https://github.com/yasm/yasm/releases/download/v1.3.0/yasm-1.3.0-win64.exe"
YASM_SHA256 = "d160b1d97266f3f28a71b4420a0ad2cd088a7977c2dd3b25af155652d8d8d91f"
OUT = ROOT / "src/android_auto/decoder/ffmpeg"
CONFIGURE_ARGS = ("--enable-cross-compile --arch=x86 --cpu=i686 --target-os=qnx --extra-cflags=-D__unix__ "
                  "--disable-everything --enable-decoder=h264 --disable-programs --disable-doc --disable-avformat "
                  "--disable-avfilter --disable-swresample --disable-swscale --disable-avdevice --disable-postproc "
                  "--disable-network --disable-iconv --disable-zlib --disable-bzlib --disable-lzma --disable-sdl "
                  "--disable-xlib --disable-debug --enable-static --disable-shared")
# fixed for QNX 6.5 / GCC 4.4.2 / binutils 2.19 (everything else the C library decides is probed in the VM)
FIXED = {
    # GCC 4.4 / binutils 2.19: no AVX/AVX2/FMA/XOP instructions in inline assembly, no __atomic builtins (the
    # __sync ones stay: HAVE_ATOMICS_GCC, HAVE_SYNC_VAL_COMPARE_AND_SWAP), no "GCC diagnostic" inside functions.
    # (The yasm-assembled functions are not affected: yasm 1.3.0 knows all of these.)
    "HAVE_AVX_INLINE": 0, "HAVE_AVX2_INLINE": 0, "HAVE_FMA3_INLINE": 0, "HAVE_FMA4_INLINE": 0, "HAVE_XOP_INLINE": 0,
    "HAVE_ATOMIC_COMPARE_EXCHANGE": 0, "HAVE_PRAGMA_DEPRECATED": 0,
    # FFmpeg's own aligned allocator on top of malloc/realloc/free instead of QNX 6.5's posix_memalign/memalign
    "HAVE_MEMALIGN": 0, "HAVE_POSIX_MEMALIGN": 0, "CONFIG_MEMALIGN_HACK": 1,
    # Linux only
    "HAVE_SCHED_GETAFFINITY": 0, "HAVE_VALGRIND_VALGRIND_H": 0, "HAVE_ASM_TYPES_H": 0, "HAVE_SYS_SOUNDCARD_H": 0,
    "HAVE_STRUCT_V4L2_FRMIVALENUM_DISCRETE": 0, "HAVE_PERL": 0, "HAVE_POD2MAN": 0,
}


def text(path):
    return path.read_bytes().replace(b"\r\n", b"\n")


def sanitize(config):
    for name, value in FIXED.items():
        config, count = re.subn(r"^#define %s [01]$" % name, "#define %s %d" % (name, value), config, flags=re.M)
        if count != 1:
            raise SystemExit("config.h: expected exactly one " + name)
    config = re.sub(r'^#define FFMPEG_CONFIGURATION ".*"$',
                    '#define FFMPEG_CONFIGURATION "%s --cc=i486-pc-nto-qnx6.5.0-gcc (config.h adjusted for QNX 6.5, '
                    'see scripts/ffmpeg_qnx.py)"' % CONFIGURE_ARGS, config, count=1, flags=re.M)
    config = re.sub(r'^#define CC_IDENT ".*"$', '#define CC_IDENT "gcc 4.4.2 (QNX 6.5.0)"', config, count=1, flags=re.M)
    return config


def sanitize_asm(config):
    for name, value in FIXED.items():
        config = re.sub(r"^%%define %s [01]$" % name, "%%define %s %d" % (name, value), config, flags=re.M)
    return config


def yasm_command(yasm, asm, obj):
    """FFmpeg's assembler command (common.mak), run in a folder with the sources and config.asm."""
    return [yasm, "-f", "elf", "-I./", "-Pconfig.asm", "-I", os.path.dirname(asm) + "/", "-o", obj, asm]


def generate(source, cfg):
    """Write config files, sources.txt and objects.txt from a configured build directory (Linux only)."""
    source, cfg = Path(source).resolve(), Path(cfg).resolve()
    if subprocess.run(["git", "-C", str(source), "rev-parse", "HEAD"], stdout=subprocess.PIPE,
                      check=True).stdout.decode().strip() != FFMPEG_COMMIT:
        raise SystemExit("FFmpeg checkout must be at " + FFMPEG_TAG)
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "config.h").write_text(sanitize((cfg / "config.h").read_text()))
    (OUT / "config.asm").write_text(sanitize_asm((cfg / "config.asm").read_text()))
    (OUT / "avconfig.h").write_bytes((cfg / "libavutil/avconfig.h").read_bytes())
    (OUT / "ffversion.h").write_bytes((cfg / "libavutil/ffversion.h").read_bytes())
    plan = subprocess.run(["make", "-n", "-B", "libavcodec/libavcodec.a", "libavutil/libavutil.a"], cwd=cfg,
                          stdout=subprocess.PIPE, stderr=subprocess.DEVNULL).stdout.decode()
    objects = []
    for obj in sorted(set(re.findall(r"-o (\S+)\.o\b", plan))):
        objects += [obj + ext for ext in (".c", ".asm") if (source / (obj + ext)).is_file()]
    include = cfg / "qnx-include"   # the adjusted config files where FFmpeg's sources look for them
    (include / "libavutil").mkdir(parents=True, exist_ok=True)
    (include / "config.h").write_bytes((OUT / "config.h").read_bytes())
    (include / "config.asm").write_bytes((OUT / "config.asm").read_bytes())
    (include / "libavutil/avconfig.h").write_bytes((OUT / "avconfig.h").read_bytes())
    (include / "libavutil/ffversion.h").write_bytes((OUT / "ffversion.h").read_bytes())
    sources = set()
    for name in objects:
        if name.endswith(".c"):
            command = ["gcc", "-m32", "-I" + str(include), "-I" + str(source), "-D_ISOC99_SOURCE",
                       "-D_FILE_OFFSET_BITS=64", "-D_LARGEFILE_SOURCE", "-D_QNX_SOURCE", "-D_POSIX_C_SOURCE=200112",
                       "-D_XOPEN_SOURCE=600", "-DHAVE_AV_CONFIG_H", "-D__unix__", "-MM", str(source / name)]
        else:
            command = ["yasm", "-f", "elf", "-I" + str(include) + "/", "-I" + str(source) + "/",
                       "-P" + str(include / "config.asm"), "-I", str((source / name).parent) + "/", "-M",
                       "-o", "x.o", str(source / name)]
        deps = subprocess.run(command, stdout=subprocess.PIPE, check=True).stdout.decode()
        for dep in deps.replace("\\\n", " ").split(":", 1)[1].split():
            path = Path(dep).resolve()
            if path.is_relative_to(source):
                sources.add(path.relative_to(source).as_posix())
        sources.add(name)
    # headers whose #include depends on a value the VM probes, and FFmpeg's licence texts
    for extra in ["libavutil/time_internal.h", "COPYING.LGPLv2.1", "LICENSE.md", "CREDITS"]:
        if (source / extra).is_file():
            sources.add(extra)
    (OUT / "objects.txt").write_text("".join(o + "\n" for o in objects))
    (OUT / "sources.txt").write_text("".join("%s  %s\n" % (hashlib.sha256(text(source / s)).hexdigest(), s)
                                             for s in sorted(sources)))
    print(len(objects), "objects (%d assembly)," % sum(o.endswith(".asm") for o in objects), len(sources), "files")


def checkout():
    """The FFmpeg checkout used by build_all.py (cloned once into .local/, only the tag)."""
    path = ROOT / ".local" / ("ffmpeg-" + FFMPEG_TAG)
    if not path.exists():
        print("cloning FFmpeg", FFMPEG_TAG, "into", path, flush=True)
        subprocess.run(["git", "-c", "core.autocrlf=false", "clone", "--depth", "1", "--branch", FFMPEG_TAG,
                        FFMPEG_GIT, str(path)], check=True)
    head = subprocess.run(["git", "-C", str(path), "rev-parse", "HEAD"], stdout=subprocess.PIPE,
                          check=True).stdout.decode().strip()
    if head != FFMPEG_COMMIT:
        raise SystemExit("%s must be at FFmpeg %s (%s)" % (path, FFMPEG_TAG, FFMPEG_COMMIT))
    return path


def yasm():
    """yasm 1.3.0: the one on PATH, or on Windows the official release binary (downloaded once, SHA-256 checked)."""
    found = shutil.which("yasm")
    if found:
        version = subprocess.run([found, "--version"], stdout=subprocess.PIPE).stdout.decode(errors="replace")
        if version.startswith("yasm 1.3.0"):
            return found
    if os.name != "nt":
        raise SystemExit("yasm 1.3.0 not found (install it, e.g. apt install yasm)")
    exe = ROOT / ".local" / "yasm-1.3.0-win64.exe"
    if not exe.exists():
        exe.parent.mkdir(exist_ok=True)
        print("downloading", YASM_URL, flush=True)
        urllib.request.urlretrieve(YASM_URL, exe)
    if hashlib.sha256(exe.read_bytes()).hexdigest() != YASM_SHA256:
        raise SystemExit("yasm: SHA-256 mismatch (%s)" % exe)
    return str(exe)


def add_ffmpeg(archive):
    """Put the FFmpeg files from sources.txt (SHA-256 checked) into the VM archive under ffmpeg/ and the assembled
    objects under ffmpeg-asm/."""
    source = checkout()
    assembler = yasm()
    key = hashlib.sha256()
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        for line in (OUT / "sources.txt").read_text().splitlines():
            digest, name = line.split("  ", 1)
            data = text(source / name)
            if hashlib.sha256(data).hexdigest() != digest:
                raise SystemExit("FFmpeg file differs from the expected one: " + name)
            archive.add("ffmpeg/" + name, data)
            key.update(digest.encode())
            (work / name).parent.mkdir(parents=True, exist_ok=True)
            (work / name).write_bytes(data)   # the checked files are the ones assembled
        (work / "config.asm").write_bytes(text(OUT / "config.asm"))
        asm = [n for n in (OUT / "objects.txt").read_text().split() if n.endswith(".asm")]
        print("assembling %d FFmpeg files with %s" % (len(asm), assembler), flush=True)
        for name in asm:
            obj = name[:-4].replace("/", "_") + ".o"
            subprocess.run(yasm_command(assembler, name, obj), cwd=work, check=True)
            data = (work / obj).read_bytes()
            archive.add("ffmpeg-asm/" + obj, data)
            key.update(hashlib.sha256(data).digest())
    for name in ("config.h", "avconfig.h", "ffversion.h", "objects.txt"):
        data = text(OUT / name)
        archive.add("ffmpeg-config/" + name, data)
        key.update(hashlib.sha256(data).digest())
    # the VM keeps the built libraries under this key, so FFmpeg is compiled again only when something changed
    key.update(hashlib.sha256(text(ROOT / "scripts/build-ffmpeg-qnx.sh")).digest())
    archive.add("build-ffmpeg-qnx.sh", text(ROOT / "scripts/build-ffmpeg-qnx.sh"), 0o700)
    archive.add("ffmpeg-config/KEY", key.hexdigest()[:16].encode() + b"\n")


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "generate":
        generate(sys.argv[2], sys.argv[3])
    else:
        raise SystemExit(__doc__)
