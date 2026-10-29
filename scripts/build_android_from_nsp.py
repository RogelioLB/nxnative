# Pack a Switch game you own for Android: your NSP / XCI files and your prod.keys in, the nxnative app and the game files on the phone out.
#   python scripts/build_android_from_nsp.py <NSP folder> -k <prod.keys> [-p <version>] [-l <language>] [--serial <adb device>] [--no-push]
#   python scripts/build_android_from_nsp.py <NSP folder> -k <prod.keys> --push-only [--serial ...]   install the app and copy a game built before, without building
#   python scripts/build_android_from_nsp.py --apk-only [--serial ...]                                  the app with every game host built on this computer, installed
#   python scripts/build_android_from_nsp.py --migrate --title <title ID> [--serial ...]               a one-game install's folder (files/game) into files/games/<title ID>
#   python scripts/build_android_from_nsp.py --check | --info <NSP folder>                              requirements / the game's details, as JSON (the desktop app)
#   --progress json: newline-delimited JSON events on stdout (build_ipa_from_nsp.py Progress), for the desktop app (scripts/nxnative_android.py)
#   Every build also writes those events to a journal, <data>/builds/<time>-<pid>.jsonl (start and exit events around them), whatever its --progress: the desktop app
#   shows a build started from a console too
# One app carries every game: its launcher (dm/android/java Launcher.java) lists the games on the phone, Android/data/org.nxnative.android/files/games/<title ID>/,
# and the app holds a host per game, lib/arm64-v8a/libdmhost_<title ID>.so, started by its loader (dm/android/loader.c). The hosts built on this computer are kept in a
# library (<data>/hosts/<title ID>/: %LOCALAPPDATA%\nxnative\android on Windows, ~/.local/share/nxnative/android elsewhere), and every APK is packed from all of them,
# so building a second game keeps the first in the app.
# Steps (the extraction and the rewrite are build_ipa_from_nsp.py's own, reused):
#   1. find the game profile from the title ID (dm/games/<id>); a game without one is surveyed and packed from a provisional profile (<NSP folder>/<id>_build/profile),
#      first with --no-push, then built and installed like the others (an experimental build: it may not boot)
#   2. extract the version to pack (the newest in the folder, or -p) with nx_extract (built with dotnet)
#   3. rewrite the code modules for Android (dm/tools/patch_nso.py with DM_TARGET=android) into <id>_build/patched_android_<version>/
#   (the shaders: split as for iOS, translated to SPIR-V for the Vulkan replay by dm/android/tools/spv_build.py)
#   4. build the game's host (dm/android, CMake + the NDK), the loader and the self test into the library, and pack the APK (dm/android/tools/build_apk.py) into <data>,
#      signed with ~/.android/nxnative-debug.jks (made on first use; every later build must use it to update the app); the button-icon font (make_ext_font.py)
#   5. unless --no-push: install the APK with adb (an update: the app's data, saves included, stay) and copy the rewritten modules, the RomFS (the extraction's
#      RomFS.img), any DLC and the translated shaders to files/games/<title ID>/, with its icon, control data, default game language (defaults.txt) and game.json.
#      A one-game install of the same game (files/game, the layout before the launcher) is moved there instead of copied again, and its save is copied by the app
# The picture comes from the Vulkan replay of NVN (dm/android/gfx_vk.c).
# Needs: Python with lz4, capstone, fonttools and backports.zstd (not on Python 3.14), the .NET SDK 10, the Android SDK with an NDK, CMake 3.22 and build-tools (ANDROID_HOME or --sdk), adb, keytool (a JDK).
import argparse, contextlib, datetime, hashlib, json, os, re, shutil, subprocess, sys, tarfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / 'scripts'))
import build_ipa_from_nsp as bi   # noqa: E402
from build_ipa_from_nsp import BuildError, say   # noqa: E402

PKG = 'org.nxnative.android'
# One signing key for every build on this computer: an update must be signed like the installed app, and uninstalling it would delete the saves and the game files on the phone
KEYSTORE = Path.home() / '.android' / 'nxnative-debug.jks'
# The letters of the button-icon font (make_ext_font.py): macOS's Arial Rounded Bold is its default; elsewhere a bold sans-serif of the system
LETTER_FONTS = ['/System/Library/Fonts/Supplemental/Arial Rounded Bold.ttf', r'C:\Windows\Fonts\arialbd.ttf', '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf']
REMOTE = f'/sdcard/Android/data/{PKG}/files'
BUILD_TOOLS, PLATFORM = '36.0.0', 'android-35'   # build_apk.py's defaults
# Games played on an Android phone (their notes say how): the desktop app shows the others as not tested there
ANDROID_TESTED = {'010015100B514000'}


def data_dir():   # the library of game hosts, the loader, the self test and the APK
    base = Path(os.environ['LOCALAPPDATA']) if os.name == 'nt' and os.environ.get('LOCALAPPDATA') else Path.home() / '.local/share'
    return base / 'nxnative' / 'android'


class Journaled(bi.Progress):   # the console's progress, and the same build's events as JSON lines in a journal file (a second Progress in json mode, written there)
    KEEP = 20   # journals kept

    def __init__(self, mode, argv):
        super().__init__(mode); self.inner = False
        d = data_dir() / 'builds'; d.mkdir(parents=True, exist_ok=True)
        for old in sorted(d.glob('*.jsonl'))[:-self.KEEP]: old.unlink(missing_ok=True)
        self.f = open(d / f'{datetime.datetime.now():%Y%m%d-%H%M%S}-{os.getpid()}.jsonl', 'a', encoding='utf-8')
        self.j = bi.Progress('json'); self.j._json = self.write
        self.write({'ev': 'start', 'pid': os.getpid(), 'id': os.environ.get('NXNATIVE_BUILD_ID'), 'argv': argv, 'cwd': os.getcwd()})

    def write(self, obj):
        obj.setdefault('t', round(datetime.datetime.now().timestamp(), 3))
        try: self.f.write(json.dumps(obj, ensure_ascii=False, default=str) + '\n'); self.f.flush()
        except (OSError, ValueError): pass   # the journal never stops the build

    def plan(self, n): super().plan(n); self.j.plan(n)
    def add(self, n): super().add(n); self.j.add(n)
    def stage(self, name, poll=None, step='', **kw):
        self.inner = True   # stage() ends the current stage and sets the counts through end() and set(): the journal's own stage() does both itself
        try: super().stage(name, None, step, **kw)
        finally: self.inner = False
        self.j.stage(name, step=step, **kw)
        if poll: self.watch(poll)
    def set(self, **kw):
        super().set(**kw)
        if not self.inner: self.j.set(**kw)
    def end(self, summary='', ok=True):
        super().end(summary, ok)
        if not self.inner: self.j.end(summary, ok)
    def say(self, msg): super().say(msg); self.j.say(msg)
    def close(self, ok=True): super().close(ok); self.j.close(ok)
    def exit(self, code): self.close(ok=code == 0); self.write({'ev': 'exit', 'code': code}); self.f.close()


def sdk_dir(arg):
    for c in (arg, os.environ.get('ANDROID_HOME'), os.environ.get('ANDROID_SDK_ROOT'), os.path.join(os.environ.get('LOCALAPPDATA', ''), 'Android', 'Sdk'), os.path.expanduser('~/Android/Sdk')):
        if c and (Path(c) / 'platform-tools').is_dir(): return Path(c)
    raise BuildError('Android SDK not found: give --sdk or set ANDROID_HOME')


def newest(d, what):
    vs = sorted((p for p in d.iterdir() if p.is_dir()), key=lambda p: [int(x) if x.isdigit() else 0 for x in p.name.split('.')]) if d.is_dir() else []
    if not vs: raise BuildError(f'no {what} in {d}')
    return vs[-1]


def build_nx_extract(work, logs, env):   # build_ipa_from_nsp.py's, with Windows' .exe
    out = work / 'tools/nx_extract'
    exe = out / ('nx_extract.exe' if os.name == 'nt' else 'nx_extract')
    bi.run(['dotnet', 'build', REPO / 'dm/tools/nx_extract', '-c', 'Release', '-o', out, '-v', 'quiet', '-nologo'], logs / 'nx_extract_build.log', env=env)
    if not exe.exists(): raise BuildError(f'nx_extract built without an executable (see {logs / "nx_extract_build.log"})')
    return exe


def build_host(sdk, game_id, work, logs, env):   # → (the game's host, the loader, the NDK)
    ndk = newest(sdk / 'ndk', 'NDK'); cm = newest(sdk / 'cmake', 'CMake') / 'bin'
    exe = '.exe' if os.name == 'nt' else ''
    bdir = work / 'android_host'
    bi.run([cm / f'cmake{exe}', '-G', 'Ninja', '-S', REPO / 'dm/android', '-B', bdir, f'-DCMAKE_MAKE_PROGRAM={cm / ("ninja" + exe)}',
            f'-DCMAKE_TOOLCHAIN_FILE={ndk / "build/cmake/android.toolchain.cmake"}', '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-29',
            f'-DGAME={game_id}', f'-DPython3_EXECUTABLE={sys.executable}'], logs / 'android_cmake.log', env=env)
    bi.run([cm / f'ninja{exe}', '-C', bdir, 'dmhost_app', 'nxloader'], logs / 'android_host.log', env=env)
    return bdir / f'libdmhost_{game_id}.so', bdir / 'libnxloader.so', ndk


def build_selftest(ndk, work, logs, env):
    bin_ = next((ndk / 'toolchains/llvm/prebuilt').iterdir()) / 'bin'; exe = '.exe' if os.name == 'nt' else ''
    d = work / 'android_selftest'; d.mkdir(parents=True, exist_ok=True)
    bi.run([sys.executable, REPO / 'dm/android/tests/selftest_nso.py', bin_ / f'clang{exe}', bin_ / f'llvm-objcopy{exe}', d / 'main.nso'], logs / 'android_selftest.log', env=env)
    bi.run([sys.executable, REPO / 'dm/tools/patch_nso.py', d / 'main.nso', d / 'patched', 'main'], logs / 'android_selftest_rewrite.log', env=env)
    return d / 'patched'


def button_font(work, logs, env):   # the Nintendo extension shared font's stand-in (hle_sdk16.c: button icons at their private-use code points), into <id>_build/font
    font = work / 'font'; ttf = font / 'nintendo_ext.ttf'
    letters = next((f for f in LETTER_FONTS if Path(f).is_file()), None)
    if not letters: raise BuildError('no letter font for the button icons: ' + ', '.join(LETTER_FONTS))
    if not bi._importable('fontTools'): raise BuildError(f'the current Python ({sys.executable}) is missing fontTools (pip install fonttools)')
    font.mkdir(parents=True, exist_ok=True)
    bi.run([sys.executable, REPO / 'dm/tools/make_ext_font.py', ttf, letters], logs / 'make_ext_font.log', env=env)
    return font


def build_shaders(sv, dirs, work, logs, env, g, dlc_roms=()):
    """The game's NVN shaders for the Vulkan replay: the split (build_ipa_from_nsp.py split_shaders, checked against the profile's tables), then
    dm/android/tools/spv_build.py (the in-repository translator, dmsh_xlate_bin's dll run by dotnet) into <work>/spv. None when there are none"""
    if not sv: return None
    sh = work / 'shader'
    own = not g.dir.resolve().is_relative_to((REPO / 'dm/games').resolve())   # a provisional profile (outside dm/games) takes the split's tables, as build_ipa_from_nsp.py does; the repository's are checked
    try: bi.split_shaders(sys.executable, REPO, [bi.rfs.source(dirs[v]) for v in sv], [dirs[v] / 'ExeFS/main' for v in sv], sh / 'pkg', env, logs, g, own, dlc_roms=dlc_roms)   # its own stage (a DLC's shaders too)
    except BuildError as e: bi.PROG.end(str(e), ok=False); say('[RED] the game is packed without shaders (its draws are skipped)'); return None
    bi.PROG.stage('shaders for Vulkan', step='translating')
    out = work / 'spv'
    try: bi.run([sys.executable, REPO / 'dm/android/tools/spv_build.py', sh / 'pkg', out, '--xlate', REPO / 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate.dll',
                 '--facts', g.table('shader_facts.tsv')], logs / 'spv_build.log', env=env)
    except BuildError as e: bi.PROG.end(str(e), ok=False); say(f'[RED] shader translation failed (see {logs / "spv_build.log"}): the game is packed with the shaders translated so far'); return out if out.is_dir() else None
    last = [l for l in (logs / 'spv_build.log').read_text(errors='replace').splitlines() if l.startswith('spv_build:')]
    bi.PROG.end(last[-1][11:] if last else str(out))
    return out


# ---------------- The library of game hosts ----------------
def library_add(gid, so, loader, selftest, meta):   # the game's host, and the loader and self test of the latest build (they do not depend on the game)
    lib = data_dir(); d = lib / 'hosts' / gid; d.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(so, d / so.name)
    (d / 'meta.json').write_text(json.dumps(dict(meta, built=datetime.datetime.now().isoformat(timespec='seconds')), indent=1), encoding='utf-8')
    (lib / 'loader').mkdir(exist_ok=True); shutil.copyfile(loader, lib / 'loader' / loader.name)
    st = lib / 'selftest'; shutil.rmtree(st, ignore_errors=True); shutil.copytree(selftest, st)


def library():   # {title ID: (host, meta)} of the hosts built on this computer
    out = {}
    for d in sorted((data_dir() / 'hosts').glob('*')):
        so = d / f'libdmhost_{d.name}.so'
        if so.is_file():
            try: meta = json.loads((d / 'meta.json').read_text(encoding='utf-8'))
            except (OSError, ValueError): meta = {}
            out[d.name] = (so, meta)
    return out


def make_apk(sdk, logs, env):   # the app with every host in the library → its path
    lib = data_dir(); hosts = library()
    loader, st = lib / 'loader' / 'libnxloader.so', lib / 'selftest'
    if not hosts or not loader.is_file() or not st.is_dir(): raise BuildError(f'no game host built on this computer yet ({lib}): build a game first')
    KEYSTORE.parent.mkdir(parents=True, exist_ok=True)
    apk = lib / 'nxnative.apk'
    bi.run([sys.executable, REPO / 'dm/android/tools/build_apk.py', '--sdk', sdk, '--loader', loader, *[x for so, _ in hosts.values() for x in ('--so', so)],
            '--selftest', st, '--keystore', KEYSTORE, '-o', apk], logs / 'android_apk.log', env=env)
    names = [m.get('name') or gid for gid, (_, m) in hosts.items()]
    say(f'app: {", ".join(names)} ({apk})')
    return apk


# ---------------- The phone ----------------
def pick_device():
    """The one device adb lists as ready ('device'): an offline Wi-Fi entry left from an earlier connection does not count"""
    out = subprocess.run(['adb', 'devices'], capture_output=True, text=True, errors='replace').stdout.splitlines()[1:]
    rows = [l.split() for l in out if l.strip()]
    ready = [r[0] for r in rows if len(r) > 1 and r[1] == 'device']
    if len(ready) == 1: return ready[0]
    listed = ', '.join(f'{r[0]} ({r[1]})' for r in rows) or 'none'
    raise BuildError(f'{len(ready)} adb devices ready ({listed}): connect one, or pick it with --serial')


def adb(serial, *a, log=None, check=True):
    cmd = ['adb'] + (['-s', serial] if serial else []) + [str(x) for x in a]
    r = subprocess.run(cmd, capture_output=True, text=True, errors='replace', env=dict(os.environ, MSYS_NO_PATHCONV='1'))
    if log: log.write_text(r.stdout + r.stderr)
    if r.returncode and check: raise BuildError(f'{" ".join(cmd)} failed: {(r.stdout + r.stderr).strip()[-400:]}')
    return r.stdout


def sh_quote(s): return "'" + s.replace("'", "'\\''") + "'"


# adb push prints no progress when its output is not a terminal: a copy's count is read from the phone while it runs (the files under the destination folder, or the
# size of the file being written), so the desktop app sees it move
def remote_files(serial, d):
    n = adb(serial, 'shell', f'find {sh_quote(d)} -type f 2>/dev/null | wc -l', check=False).strip()
    return int(n) if n.isdigit() else None


def remote_size(serial, f):
    n = adb(serial, 'shell', f'stat -c %s {sh_quote(f)} 2>/dev/null', check=False).strip()
    return int(n) if n.isdigit() else None


@contextlib.contextmanager
def counting(note, total, unit, read):   # while the block runs: done = read() (None: not known yet) of total, in unit ('B': bytes)
    bi.PROG.set(done=0, total=total, unit=unit, note=note)
    def poll():
        v = read()
        return None if v is None else {'done': min(v, total)}
    bi.PROG.watch(poll)
    try: yield
    finally: bi.PROG.watch(None)


TAR_FILES = 200   # a folder with more files than this goes over as one tar archive (push_tar)
MANIFEST = '.nxnative_manifest'   # in a folder copied as a tar: each file's size and modification time, for the next copy to send only what changed


def push_tar(serial, local, parent, name, files, log, sync):
    # adb push sends a folder file by file: Super Mario Bros. Wonder's 37,393 shader files took over 20 minutes to a phone that had none (about 30 files a second).
    # One tar archive, unpacked by the phone's tar, takes a minute or two. With sync, the manifest a copy left in the folder keeps it incremental, as push --sync was
    dst = f'{parent}/{name}'
    rows = {f.relative_to(local).as_posix(): f'{f.stat().st_size} {f.stat().st_mtime_ns}' for f in files}
    old = {}
    if sync:
        for line in adb(serial, 'shell', f'cat {sh_quote(dst + "/" + MANIFEST)} 2>/dev/null', check=False).splitlines():
            meta, _, rel = line.rstrip('\r').partition('\t')
            if rel: old[rel] = meta
    send = [r for r in rows if old.get(r) != rows[r]]
    adb(serial, 'shell', 'mkdir', '-p', dst)
    if send:
        tar = log.with_suffix('.tar')
        bi.PROG.set(done=0, total=len(send), unit='files', note='packing')
        with tarfile.open(tar, 'w', format=tarfile.USTAR_FORMAT) as t:
            for i, r in enumerate(send, 1):
                ti = t.gettarinfo(local / r, arcname=r); ti.mode = 0o666; ti.uid = ti.gid = 0; ti.uname = ti.gname = ''
                with open(local / r, 'rb') as fh: t.addfile(ti, fh)
                bi.PROG.set(done=i)
        remote = f'{parent}/.{name}.tar'
        try:
            with counting('sending', tar.stat().st_size, 'B', lambda: remote_size(serial, remote)):
                adb(serial, 'push', tar, remote, log=log)
            have = len(rows) - len(send)   # the files kept from an earlier copy are counted by the phone too
            with counting('unpacking on the phone', len(send), 'files', lambda: (lambda n: None if n is None else max(n - have, 0))(remote_files(serial, dst))):
                adb(serial, 'shell', f'tar -xf {sh_quote(remote)} -C {sh_quote(dst)}')
        finally:
            adb(serial, 'shell', 'rm', '-f', remote, check=False); tar.unlink(missing_ok=True)
    man = log.with_suffix('.manifest'); man.write_text(''.join(f'{v}\t{k}\n' for k, v in sorted(rows.items())), encoding='utf-8', newline='\n')
    adb(serial, 'push', man, f'{dst}/{MANIFEST}')
    say(f'{name}: {len(send):,} of {len(rows):,} files sent in one archive' + ('' if len(send) == len(rows) else ' (the others were on the phone already)'))


CHUNK = 256 << 20   # push_resumable's piece


def remote_hash(serial, remote, a, n):   # SHA-256 of n bytes at offset a (a multiple of 4 KiB) of a file on the phone, or None
    out = adb(serial, 'shell', f'dd if={sh_quote(remote)} bs=4096 skip={a // 4096} count={(n + 4095) // 4096} 2>/dev/null | head -c {n} | sha256sum', check=False).split()[:1]
    return out[0] if out else None


def same_ends(serial, local, remote):   # the first and last MiB of a local file and of the same-size file on the phone hash the same
    size = local.stat().st_size
    with open(local, 'rb') as f:
        for a in sorted({0, max(0, size - (1 << 20)) // 4096 * 4096}):
            f.seek(a); n = min(1 << 20, size - a)
            if remote_hash(serial, remote, a, n) != hashlib.sha256(f.read(n)).hexdigest(): return False
    return True


def push_resumable(serial, local, remote, log):
    # A large file (a game's RomFS.img, gigabytes) in pieces appended to <remote>.partial, renamed when whole: adb push cannot resume a file, and wireless adb dropped
    # twice in the middle of Bowser's Fury's 2.8 GB image, each time from zero. A copy cut short goes on from what the phone holds, once the last MiB of it matches
    size, part = local.stat().st_size, remote + '.partial'
    have = remote_size(serial, part) or 0
    if have > size: have = 0
    if have:
        a = max(0, have - (1 << 20)) // 4096 * 4096   # the phone's last MiB (from a 4 KiB boundary), hashed on both sides
        theirs = adb(serial, 'shell', f'dd if={sh_quote(part)} bs=4096 skip={a // 4096} count={(have - a + 4095) // 4096} 2>/dev/null | head -c {have - a} | sha256sum', check=False).split()[:1]
        with open(local, 'rb') as f: f.seek(a); ours = hashlib.sha256(f.read(have - a)).hexdigest()
        if theirs != [ours]: say(f'{remote}: the part on the phone ({have:,} bytes) differs from this file: sent again from the start'); have = 0
        else: say(f'{remote}: {have:,} of {size:,} bytes on the phone already: going on from there')
    if not have: adb(serial, 'shell', f': > {sh_quote(part)}')
    piece, tmp = log.with_suffix('.piece'), remote + '.piece'
    with counting('sending', size, 'B', lambda: (lambda a, b: None if a is None else a + (b or 0))(remote_size(serial, part), remote_size(serial, tmp))):
        with open(local, 'rb') as f:
            f.seek(have)
            while have < size:
                data = f.read(min(CHUNK, size - have)); piece.write_bytes(data)
                adb(serial, 'push', piece, tmp, log=log)
                adb(serial, 'shell', f'cat {sh_quote(tmp)} >> {sh_quote(part)} && rm {sh_quote(tmp)}')
                have += len(data)
    piece.unlink(missing_ok=True)
    if remote_size(serial, part) != size: raise BuildError(f'{part} is not {size:,} bytes after the copy: run the install again')
    adb(serial, 'shell', f'mv -f {sh_quote(part)} {sh_quote(remote)}')


def push_dir(serial, local, parent, name, log, sync=False):
    files = [f for f in local.rglob('*') if f.is_file()]
    if len(files) > TAR_FILES: return push_tar(serial, local, parent, name, files, log, sync)
    # <local folder> → <parent>/<name>: adb push copies a folder by its own name into the remote parent ("<folder>/." makes "/./" paths that adb cannot create
    # subfolders under on Windows); a folder named otherwise is pushed under its own name and renamed. adbd may not create folders under Android/data (its FUSE
    # mount refuses them: "secure_mkdirs() failed"), while the shell may: the whole tree is made first by a mkdir script run in the shell
    dirs = [f'{parent}/{local.name}'] + [f'{parent}/{local.name}/{d.relative_to(local).as_posix()}' for d in sorted(local.rglob('*')) if d.is_dir()]
    script = log.with_suffix('.mkdirs.sh')
    script.write_text(''.join(f'mkdir -p {sh_quote(d)}\n' for d in dirs), newline='\n')
    adb(serial, 'push', script, '/data/local/tmp/nxnative_mkdirs.sh'); adb(serial, 'shell', 'sh', '/data/local/tmp/nxnative_mkdirs.sh')
    with counting('sending', len(files), 'files', lambda: remote_files(serial, dirs[0])):
        adb(serial, 'push', *(['--sync'] if sync else []), local, f'{parent}/', log=log)
    if local.name != name:
        adb(serial, 'shell', 'rm', '-rf', f'{parent}/{name}'); adb(serial, 'shell', 'mv', f'{parent}/{local.name}', f'{parent}/{name}')


def open_tree(serial, d):
    # adb makes the folders as the shell user, drwxrws--- (group ext_data_rw): on Android 15 (a Galaxy S21 Ultra) the app could not enter them, found no game and ran its self test.
    # Opened to everyone; Android/data/<package> is reachable only by the app and the shell, so this gives no other app access
    # One chmod -R (rw for files, rwx for folders: X): find -exec chmod {} + overflowed the argument list on 37,000 shader files. The games folder too, which adb made the same way
    # A file the app made there itself (launcher.txt, its settings) is the app's: the shell may not chmod it ("Operation not permitted"), and the app needs no help to read it
    out = adb(serial, 'shell', f"chmod -R a+rwX {sh_quote(d)} 2>&1; chmod a+rwx {sh_quote(d.rsplit('/', 1)[0])} 2>&1", check=False)
    bad = [ln for ln in out.splitlines() if ln.strip() and 'Operation not permitted' not in ln]
    if bad: raise BuildError(f'opening {d} to the app failed: {"; ".join(bad)[:400]}')


def install(serial, apk, logs):   # an update (-r): the app's data, saves included, stay. Never uninstalled here: that would delete the saves
    if not KEYSTORE.is_file(): raise BuildError(f'no signing key {KEYSTORE}: the app on the phone could not be updated')
    try: adb(serial, 'install', '-r', apk, log=logs / 'adb_install.log')
    except BuildError as e:
        if 'INSTALL_FAILED_UPDATE_INCOMPATIBLE' in str(e):
            raise BuildError(f'the nxnative app on the phone was signed with another key than {KEYSTORE}. Uninstalling it deletes its saves and game files: '
                             f'back up {REMOTE} and the saves first (the launcher\'s Back up saves), then uninstall it by hand and run this again') from e
        raise


def remote_exists(serial, path): return adb(serial, 'shell', f'[ -e {sh_quote(path)} ] && echo yes', check=False).strip() == 'yes'


def migrate_legacy(serial, gid, nacp_local=None):
    """The one-game layout (files/game, before the launcher) → files/games/<gid>, with a legacy_save marker: the app then copies its internal files/save to saves/<gid>
    (Launcher.java migrate, after a backup; files/save itself is left as it was). With nacp_local, only when the phone's game is that one (same control.nacp)
    → whether it moved"""
    old, new = f'{REMOTE}/game', f'{REMOTE}/games/{gid}'
    if not remote_exists(serial, f'{old}/code/main.fixups.tsv'): return False
    if nacp_local is not None:
        theirs = adb(serial, 'shell', f'sha256sum {sh_quote(old + "/control.nacp")}', check=False).split()[:1]
        if theirs != [hashlib.sha256(Path(nacp_local).read_bytes()).hexdigest()]:
            say(f'[RED] the phone has another game in {old} (the layout before the launcher): left as it is; move it with --migrate --title <its title ID>'); return False
    if remote_exists(serial, new): raise BuildError(f'{new} exists already: the old {old} is left as it is (remove one of them by hand)')
    adb(serial, 'shell', f'mkdir -p {sh_quote(REMOTE + "/games")} && mv {sh_quote(old)} {sh_quote(new)} && touch {sh_quote(new + "/legacy_save")}')
    open_tree(serial, new)
    say(f'moved the one-game install {old} to {new}: the app copies its save to that game when it next opens (after a backup in files/backups/)')
    return True


def push_game(serial, gid, game, patched, spv, font, aoc, lang, work, logs, meta):
    """The game's files → files/games/<gid>/ (the RomFS with --sync: a repeat copies only what changed)"""
    dst = f'{REMOTE}/games/{gid}'
    parts = ['the one-game install', 'font', 'code'] + ['shaders'] * bool(spv) + ['RomFS'] + ['DLC'] * aoc.is_dir() + ['launcher files', 'permissions']
    def part(name, step=None):   # the part in the step's name; the count is the part's files or bytes (counting), or the parts when it has none
        bi.PROG.set(step=f'{step or name} · part {parts.index(name) + 1} of {len(parts)}'); bi.PROG.set(done=parts.index(name), total=len(parts), unit='parts')
    part('the one-game install', 'looking for the one-game install'); migrate_legacy(serial, gid, game / 'control.nacp')
    adb(serial, 'shell', 'rm', '-rf', f'{dst}/code', f'{dst}/font', f'{dst}/{patched.name}'); adb(serial, 'shell', 'mkdir', '-p', dst)
    part('font'); push_dir(serial, font, dst, 'font', logs / 'adb_push_font.log')
    part('code'); push_dir(serial, patched, dst, 'code', logs / 'adb_push_code.log')
    if spv: part('shaders', 'shaders (only the changed files)'); push_dir(serial, spv, dst, 'spv', logs / 'adb_push_spv.log', sync=True)
    part('RomFS', 'RomFS (a large game takes a while; only a changed image is copied again)')
    rom = bi.rfs.source(game)   # the extraction's RomFS image (RomFS.img: the host reads it as it is, hle_fs.c), or a RomFS folder from an older extraction
    if bi.rfs.is_image(rom):   # not rom.is_file(): romfs.Path takes an image as its root folder
        # a RomFS.img folder left by an earlier push would make adb fail ("Is a directory"); a RomFS.img file stays, for --sync
        adb(serial, 'shell', f"rm -rf {sh_quote(dst + '/RomFS')}; if [ -d {sh_quote(dst + '/RomFS.img')} ]; then rm -rf {sh_quote(dst + '/RomFS.img')}; fi")
        # there whole, the same size and the same first and last MiB: kept. Not adb push --sync, which also compares modification times: push_resumable's image is
        # assembled on the phone (cat >>), so its time is the copy's, and --sync sent the whole image again on every install (Danganronpa V3's 6.7 GB)
        if remote_size(serial, f'{dst}/RomFS.img') == os.path.getsize(str(rom)) and same_ends(serial, Path(str(rom)), f'{dst}/RomFS.img'):
            say('RomFS.img: on the phone already (same size, first and last MiB)')
        else: push_resumable(serial, Path(str(rom)), f'{dst}/RomFS.img', logs / 'adb_push_romfs.log')
    else:
        adb(serial, 'shell', 'rm', '-f', f'{dst}/RomFS.img')
        push_dir(serial, rom, dst, 'RomFS', logs / 'adb_push_romfs.log', sync=True)
    if aoc.is_dir(): part('DLC'); push_dir(serial, aoc, dst, 'aoc', logs / 'adb_push_aoc.log', sync=True)
    part('launcher files')
    # the launcher (dm/android/java Launcher.java): the game's icon and control data (name, version, languages), its defaults (the language picked with --language) and
    # game.json; env.txt is the player's and stays as it is (a key it sets wins over the settings screen)
    for f in ('icon.jpg', 'control.nacp'):
        if (game / f).is_file(): adb(serial, 'push', game / f, f'{dst}/{f}')
    dflt = work / 'android_defaults.txt'; dflt.write_text(f'DM_LANG={lang}\n')
    adb(serial, 'push', dflt, f'{dst}/defaults.txt')
    gj = work / 'android_game.json'; gj.write_text(json.dumps(meta, indent=1), encoding='utf-8')
    adb(serial, 'push', gj, f'{dst}/game.json')
    part('permissions'); open_tree(serial, dst)


# ---------------- --check and --info (JSON for the desktop app) ----------------
def tool_version(cmd):
    try: r = subprocess.run(cmd, capture_output=True, text=True, errors='replace', timeout=30)
    except (OSError, subprocess.TimeoutExpired): return None
    return (r.stdout + r.stderr).strip() if r.returncode == 0 else None


def check(sdk_arg):
    rows = []
    def row(name, ok, detail, fix=''): rows.append({'name': name, 'ok': bool(ok), 'detail': detail, 'fix': '' if ok else fix})
    for m, pip in (('lz4.block', 'lz4'), ('capstone', 'capstone'), ('fontTools', 'fonttools'), ('PySide6', 'PySide6')):
        row(f'Python module {pip}', bi._importable(m), sys.executable, f'{sys.executable} -m pip install {pip}')
    z = bi._importable('compression.zstd') or bi._importable('backports.zstd')
    row('Python zstd', z, sys.executable, f'{sys.executable} -m pip install backports.zstd (or Python 3.14+)')
    env = dict(os.environ)
    try: bi.configure_dotnet(env); dn = tool_version(['dotnet', '--version'])
    except BuildError as e: dn = None; row('.NET SDK 10', False, str(e), 'install the .NET 10 SDK: https://dotnet.microsoft.com/download')
    else: row('.NET SDK 10', dn and dn.split('.')[0] == '10', dn or 'not found', 'install the .NET 10 SDK: https://dotnet.microsoft.com/download')
    try: sdk = sdk_dir(sdk_arg)
    except BuildError as e: sdk = None; row('Android SDK', False, str(e), 'install Android Studio or the command-line tools, then set ANDROID_HOME')
    else: row('Android SDK', True, str(sdk))
    if sdk:
        for what, d in (('NDK', sdk / 'ndk'), ('CMake', sdk / 'cmake')):
            try: row(what, True, str(newest(d, what)))
            except BuildError as e: row(what, False, str(e), f'sdkmanager "{"ndk;28.2.13676358" if what == "NDK" else "cmake;3.22.1"}"')
        row(f'build-tools {BUILD_TOOLS}', (sdk / 'build-tools' / BUILD_TOOLS).is_dir(), str(sdk / 'build-tools' / BUILD_TOOLS), f'sdkmanager "build-tools;{BUILD_TOOLS}"')
        row(f'platform {PLATFORM}', (sdk / 'platforms' / PLATFORM / 'android.jar').is_file(), str(sdk / 'platforms' / PLATFORM), f'sdkmanager "platforms;{PLATFORM}"')
    a = tool_version(['adb', 'version'])
    row('adb', a, a.splitlines()[0] if a else 'not found', 'install the Android SDK platform-tools and put them on PATH')
    for t in ('keytool', 'javac'):
        v = shutil.which(t); row(f'JDK {t}', v, v or 'not found', 'install a JDK (17 or newer) and put its bin folder on PATH')
    row('signing key', True, f'{KEYSTORE}' + ('' if KEYSTORE.is_file() else ' (made at the first build)'))
    hosts = library(); row('game hosts', True, f'{len(hosts)} in {data_dir() / "hosts"}' + (': ' + ', '.join(hosts) if hosts else ''))
    return rows


def games_table():   # {title ID: {engine, graphics, build, sdk}} from dm/games/README.md's Games table
    out = {}
    for line in (REPO / 'dm/games/README.md').read_text(encoding='utf-8').splitlines():
        c = [x.strip() for x in line.strip().strip('|').split('|')]
        if len(c) == 6 and re.fullmatch(r'`[0-9A-F]{16}`', c[1]):
            out[c[1].strip('`')] = {'build': c[2], 'sdk': c[3], 'engine': c[4], 'graphics': c[5]}
    return out


def provisional_profile(gamemod, nsp_dir, app):   # the provisional profile an experimental build wrote (<NSP folder>/<id>_build/profile/<id>), joined to game.py's search path; None before the survey
    pdir = nsp_dir / f'{app:016X}_build' / 'profile'
    if not (pdir / f'{app:016X}' / 'game.tsv').is_file(): return None
    os.environ['DM_GAMES_DIR'] = os.pathsep.join([str(pdir)] + [d for d in os.environ.get('DM_GAMES_DIR', '').split(os.pathsep) if d and d != str(pdir)])
    return gamemod.load(f'{app:016X}')


def info(nsp_dir):
    sys.path.insert(0, str(REPO / 'dm/games')); import game as gamemod
    sys.path.insert(0, str(REPO / 'dm/tools')); import nacp
    app, fam = bi.scan_nsps(nsp_dir)
    tid = f'{app:016X}'; g = gamemod.by_title(app); prov = False
    if g is None: g = provisional_profile(gamemod, nsp_dir, app); prov = g is not None   # an experimental build's: its art, versions and host show like a profiled game's
    out = {'title_id': tid, 'folder': str(nsp_dir), 'profile': bool(g), 'provisional': prov, 'name': g.get('name', '') if g else '', 'table': games_table().get(tid),
           'android_tested': tid in ANDROID_TESTED, 'dlc': len(fam['dlc']),
           'files': [{'name': p.name, 'bytes': p.stat().st_size} for p in [fam['base'], *[p for _, p in fam['updates']], *[p for _, _, p in fam['dlc']]] if p]}
    if g:
        have = bi.nsp_versions(fam, g.builds())
        out['versions'] = sorted(have, key=bi.vnum); out['newest'] = max(have, key=bi.vnum)
        work = nsp_dir / f'{g.id}_build'; ex = work / 'extract'
        out['extracted'] = {}
        for v in out['versions']:
            d = ex / v
            if (d / 'control.nacp').is_file():
                rom = bi.rfs.source(d)
                out['extracted'][v] = {'icon': str(d / 'icon.jpg') if (d / 'icon.jpg').is_file() else None, 'name': nacp.name(d / 'control.nacp'),
                                       'display': nacp.version(d / 'control.nacp'), 'languages': nacp.languages(d / 'control.nacp'),
                                       'romfs_bytes': Path(rom).stat().st_size if bi.rfs.is_image(rom) else None,
                                       'built': (work / f'patched_android_{v}' / 'main.fixups.tsv').is_file()}
        h = library().get(g.id); out['host'] = h[1] if h else None
    return out


# ---------------- main ----------------
def main(argv):
    ap = argparse.ArgumentParser(description='Pack a Switch game you own for Android (the nxnative app + the game files on the phone)')
    ap.add_argument('nsp_dir', nargs='?'); ap.add_argument('-k', '--keys', help='your prod.keys')
    ap.add_argument('-p', '--package', help='the version to pack (v0 = the base game; default: the newest in the folder)')
    ap.add_argument('-l', '--language', default='en-US', help='the game language (a code or a name; English by default)')
    ap.add_argument('--sdk', help='the Android SDK (default ANDROID_HOME)'); ap.add_argument('--serial', help='the adb device (default: the only one)')
    ap.add_argument('--no-push', action='store_true', help='build the APK only; do not install it or copy the game files to the phone')
    ap.add_argument('--push-only', action='store_true', help='install the app and copy a game built before to the phone, without building')
    ap.add_argument('--apk-only', action='store_true', help='pack the app with every game host built on this computer and install it')
    ap.add_argument('--migrate', action='store_true', help="move a one-game install's files/game to files/games/<--title> on the phone")
    ap.add_argument('--title', help='the title ID for --migrate')
    ap.add_argument('--check', action='store_true', help='print the requirements as JSON and stop')
    ap.add_argument('--info', action='store_true', help="print the NSP folder's game details as JSON and stop")
    ap.add_argument('--progress', choices=('auto', 'bar', 'lines', 'json', 'off'), default='auto', help='stage lines on the console; json = newline-delimited JSON events')
    ap.add_argument('--allow-damaged', action='store_true', help='pack files that fail the NCA hash check as read')
    a = ap.parse_args(argv)
    bi.PROG.mode = bi.progress_mode('off' if a.check or a.info else a.progress); bi.ALLOW_DAMAGED = a.allow_damaged
    if not (a.check or a.info) and os.environ.get('NXNATIVE_JOURNAL', '1') != '0': bi.PROG = Journaled(bi.PROG.mode, argv)
    if a.check or a.info:   # stdout carries the JSON alone: messages on the way (say) go to stderr
        if a.info and not a.nsp_dir: raise BuildError('--info needs the NSP folder')
        with contextlib.redirect_stdout(sys.stderr): out = check(a.sdk) if a.check else info(Path(a.nsp_dir).expanduser().resolve())
        print(json.dumps(out, indent=1)); return
    if a.apk_only or a.migrate:
        logs = data_dir() / 'logs'; logs.mkdir(parents=True, exist_ok=True)
        serial = a.serial or os.environ.get('ANDROID_SERIAL') or pick_device()
        if a.migrate:
            if not a.title or not re.fullmatch(r'[0-9A-Fa-f]{16}', a.title): raise BuildError('--migrate needs --title <the game\'s 16-digit title ID>')
            bi.PROG.plan(1); bi.PROG.stage('move the one-game install')
            if not migrate_legacy(serial, a.title.upper()): say(f'nothing to move: no game in {REMOTE}/game')
            bi.PROG.end(); return
        bi.PROG.plan(2); env = dict(os.environ)
        bi.PROG.stage('APK'); apk = make_apk(sdk_dir(a.sdk), logs, env); bi.PROG.end(str(apk))
        bi.PROG.stage('install'); install(serial, apk, logs); bi.PROG.end(serial)
        return
    if not a.nsp_dir or not a.keys: raise BuildError('give the NSP folder and -k <prod.keys> (or --check / --apk-only / --migrate)')
    nsp_dir, keys = Path(a.nsp_dir).expanduser().resolve(), Path(a.keys).expanduser().resolve()
    if not nsp_dir.is_dir(): raise BuildError(f'not a folder: {nsp_dir}')
    if not keys.is_file(): raise BuildError(f'no such file: {keys}')
    missing = [m for m in ('lz4.block', 'capstone') if not bi._importable(m)]
    if missing: raise BuildError(f'the current Python ({sys.executable}) is missing modules: {", ".join(missing)} (pip install lz4 capstone)')
    if not (bi._importable('compression.zstd') or bi._importable('backports.zstd')):   # the shader split decompresses zstd files (Python 3.14's compression.zstd, or its backport)
        raise BuildError(f'the current Python ({sys.executable}) has no zstd module: pip install backports.zstd (or use Python 3.14 or newer)')
    sys.path.insert(0, str(REPO / 'dm/games')); import game as gamemod
    import profile_guess; bi.pg = profile_guess
    sys.path.insert(0, str(REPO / 'dm/tools')); import nacp; bi.nacp = nacp
    sdk = sdk_dir(a.sdk)
    env = dict(os.environ); bi.configure_dotnet(env)
    app, fam = bi.scan_nsps(nsp_dir)
    fam = bi.decompress_nsz(nsp_dir, app, fam, keys, env)
    g = gamemod.by_title(app)
    if g is None:   # no game profile: surveyed and packed from a provisional profile outside the repository, as build_ipa_from_nsp.py does (the game may not boot)
        pdir = nsp_dir / f'{app:016X}_build' / 'profile'
        if (pdir / f'{app:016X}' / 'game.tsv').exists(): say(f'no game profile for {app:016X}: reusing the provisional profile from the last run (delete it to survey again)')
        else:   # the first experimental build only packs; once its provisional profile exists, it is built and installed like a profiled game's (to try it on a phone)
            if not a.no_push: raise BuildError(f'no game profile for {app:016X}: a game without one is first packed with --no-push (a survey, a provisional profile and an APK that may not boot)')
            ipa_nx, bi.build_nx_extract = bi.build_nx_extract, lambda repo, work, logs, env: build_nx_extract(work, logs, env)   # this script's, with Windows' .exe
            try: facts = bi.survey(nsp_dir, app, fam, keys, REPO, sys.executable, env)
            finally: bi.build_nx_extract = ipa_nx
            facts['survey'] = str(nsp_dir / f'{app:016X}_survey' / 'survey.md')
            bi.write_profile(pdir / f'{app:016X}', app, facts)
        g = provisional_profile(gamemod, nsp_dir, app); env['DM_GAMES_DIR'] = os.environ['DM_GAMES_DIR']
        if g is None: raise BuildError(f'no provisional profile in {pdir}: the survey wrote none')
        say('[RED] provisional game profile: values guessed from the survey, the game may not boot; bring it up per docs/agents after packing')
    say(f'game profile: {g.id} ({g.get("name", "")})')
    builds = g.builds(); have = bi.nsp_versions(fam, builds)
    pkg_ver = a.package or max(have, key=bi.vnum)
    if pkg_ver not in have: raise BuildError(f'version {pkg_ver} is not in the folder, which holds {", ".join(sorted(have, key=bi.vnum))}')
    work = nsp_dir / f'{g.id}_build'; logs = work / 'logs'; ex = work / 'extract'
    ex.mkdir(parents=True, exist_ok=True); (work / 'tmp').mkdir(exist_ok=True)
    env.update(DM_GAME=g.id, DM_ROOT=str(work), WORK=str(work / 'tmp'), DM_TARGET='android', PYTHONUTF8='1')   # the tools' text files in UTF-8 on Windows too (its default is the ANSI code page)
    patched = work / f'patched_android_{pkg_ver}'
    if a.push_only:   # what an earlier build left: the extraction, the rewrite, the shaders, the font and the host in the library
        game, spv, font = ex / pkg_ver, work / 'spv', work / 'font'
        need = [p for p in (game / 'control.nacp', patched / 'main.fixups.tsv', font) if not p.exists()]
        if need or g.id not in library(): raise BuildError(f'{g.id} {pkg_ver} was not built on this computer yet ({", ".join(map(str, need)) or "no host in the library"}): build it first')
        if not spv.is_dir(): spv = None
        bi.PROG.plan(3)
    else:
        # the shaders are split from every version the profile's shader tables were made from (shader_versions), as the iOS build does
        sv = g.words('shader_versions') or [pkg_ver]
        if not all(v in have for v in sv): say(f'[RED] the folder has no {", ".join(v for v in sv if v not in have)}, whose files the profile shader tables were made from: no shaders are translated'); sv = []
        picks = bi.pick_nsps(fam, have, sorted(set(sv) | {pkg_ver}, key=bi.vnum))
        bi.PROG.plan(5 + 2 * bool(sv) + len(set(sv) - {pkg_ver}) + bool(fam['dlc']) + (not a.no_push))   # an extraction per other shader version, and the DLC's
        bi.PROG.stage('build nx_extract'); nx = build_nx_extract(work, logs, env); bi.PROG.end()
        b, u = picks[pkg_ver]
        game = bi.extract(nx, keys, ex, pkg_ver, b, u, None, logs, g)
        bad = bi.build_mismatch(game, pkg_ver, g)
        if bad: say(f'[RED] not the builds game profile {g.id} registers ({"; ".join(bad)}): the per-build fixes of the profile do not reach these modules, the game may not boot')
        g = bi.rewrite(sys.executable, REPO, game / 'ExeFS', patched, env, logs, g)
        # the DLC in the folder, as for iOS: <id>_build/aoc/<index>.img, which push_game copies to the game's aoc/ and the host mounts and reports as owned (DM_AOC_DIR).
        # It was not extracted for Android: The Binding of Isaac's Repentance (DLC 010021C000B6B001) started as Afterbirth+
        aoc = bi.extract_aoc(nx, keys, REPO, work, app, fam, logs)
        dlc_roms = sorted(p for p in aoc.iterdir() if p.is_dir() or p.suffix == '.img') if aoc else []
        spv = build_shaders(sv, {v: (game if v == pkg_ver else bi.extract(nx, keys, ex, v, picks[v][0], picks[v][1], None, logs, g)) for v in sv}, work, logs, env, g, dlc_roms)
        bi.PROG.stage('Android host'); so, loader, ndk = build_host(sdk, g.id, work, logs, env); st = build_selftest(ndk, work, logs, env)
        library_add(g.id, so, loader, st, {'title_id': g.id, 'name': nacp.name(game / 'control.nacp') or g.get('name', ''), 'version': pkg_ver,
                                            'display': nacp.version(game / 'control.nacp'), 'nsp_dir': str(nsp_dir)})
        bi.PROG.end(str(so))
        font = button_font(work, logs, env)
    have_l = nacp.languages(game / 'control.nacp')
    lang = bi.game_language(bi.language_code(a.language) or 'en-US', a.language != 'en-US', have_l)
    bi.PROG.stage('APK'); apk = make_apk(sdk, logs, env); bi.PROG.end(str(apk))
    label = nacp.name(game / 'control.nacp') or g.get('name', '') or g.id
    if a.no_push:
        say(f'done: {apk}. On the phone: install it, then copy {patched} to {REMOTE}/games/{g.id}/code and {bi.rfs.source(game)} to {REMOTE}/games/{g.id}/'); return
    bi.PROG.stage('install and copy to the phone')
    serial = a.serial or os.environ.get('ANDROID_SERIAL') or pick_device()
    bi.PROG.set(step='installing the app (an update: its data stays)'); install(serial, apk, logs)
    adb(serial, 'shell', 'am', 'force-stop', PKG)
    rom = bi.rfs.source(game)
    push_game(serial, g.id, game, patched, spv, font, work / 'aoc', lang, work, logs,
              {'title_id': g.id, 'name': label, 'version': pkg_ver, 'display': nacp.version(game / 'control.nacp'),
               'romfs_bytes': Path(rom).stat().st_size if bi.rfs.is_image(rom) else None})
    bi.PROG.end(f'{REMOTE}/games/{g.id}')
    say(f'done: open nxnative on the phone and pick "{label}". Log: {REMOTE}/log.txt (adb logcat -s nxnative)')


if __name__ == '__main__':
    code = 1
    try: main(sys.argv[1:]); code = 0
    except BuildError as e:
        bi.PROG.close(ok=False); bi.PROG.say(f'[RED] {e}'); sys.exit(1)
    except SystemExit as e: code = e.code if isinstance(e.code, int) else (0 if e.code is None else 1); raise
    except KeyboardInterrupt: code = 130; raise
    finally:
        bi.PROG.close(ok=False)
        if isinstance(bi.PROG, Journaled): bi.PROG.exit(code)
