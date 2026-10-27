# Pack a Switch game you own for Android: your NSP / XCI files and your prod.keys in, an APK and the game files on the phone out.
#   python scripts/build_android_from_nsp.py <NSP folder> -k <prod.keys> [-p <version>] [-l <language>] [--serial <adb device>] [--no-push]
# Steps (the extraction and the rewrite are build_ipa_from_nsp.py's own, reused):
#   1. find the game profile from the title ID (dm/games/<id>; only games with a profile: a provisional profile needs the Mac host's heap check)
#   2. extract the version to pack (the newest in the folder, or -p) with nx_extract (built with dotnet)
#   3. rewrite the code modules for Android (dm/tools/patch_nso.py with DM_TARGET=android) into <id>_build/patched_android_<version>/
#   4. build the Android host for the profile (dm/android, CMake + the NDK) and the self test, and pack the APK (dm/android/tools/build_apk.py) next to the NSPs,
#      signed with ~/.android/nxnative-debug.jks (made on first use; every later build must use it to update the app); the button-icon font (make_ext_font.py)
#   5. unless --no-push: install the APK with adb and copy the rewritten modules, the RomFS, any DLC and the translated shaders to the app's folder on the phone
#   (the shaders: split as for iOS, translated to SPIR-V for the Vulkan replay by dm/android/tools/spv_build.py)
#      (Android/data/org.nxnative.android/files/game/; the RomFS as the extraction's RomFS.img), with its icon, control data and default game language (game/defaults.txt) for the settings screen
# The picture comes from the Vulkan replay of NVN (dm/android/gfx_vk.c); the app opens on a settings screen (dm/android/java Launcher.java), then the game.
# Needs: Python with lz4, capstone and fonttools, the .NET SDK 10, the Android SDK with an NDK, CMake 3.22 and build-tools (ANDROID_HOME or --sdk), adb, keytool (a JDK).
import argparse, os, subprocess, sys
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


def build_host(sdk, game_id, work, logs, env):
    ndk = newest(sdk / 'ndk', 'NDK'); cm = newest(sdk / 'cmake', 'CMake') / 'bin'
    exe = '.exe' if os.name == 'nt' else ''
    bdir = work / 'android_host'
    bi.run([cm / f'cmake{exe}', '-G', 'Ninja', '-S', REPO / 'dm/android', '-B', bdir, f'-DCMAKE_MAKE_PROGRAM={cm / ("ninja" + exe)}',
            f'-DCMAKE_TOOLCHAIN_FILE={ndk / "build/cmake/android.toolchain.cmake"}', '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-29',
            f'-DGAME={game_id}', f'-DPython3_EXECUTABLE={sys.executable}'], logs / 'android_cmake.log', env=env)
    bi.run([cm / f'ninja{exe}', '-C', bdir, 'dmhost_app'], logs / 'android_host.log', env=env)
    return bdir / 'libdmhost.so', ndk


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


def build_shaders(sv, dirs, work, logs, env, g):
    """The game's NVN shaders for the Vulkan replay: the split (build_ipa_from_nsp.py split_shaders, checked against the profile's tables), then
    dm/android/tools/spv_build.py (the in-repository translator, dmsh_xlate_bin's dll run by dotnet) into <work>/spv. None when there are none"""
    if not sv: return None
    sh = work / 'shader'
    try: bi.split_shaders(sys.executable, REPO, [bi.rfs.source(dirs[v]) for v in sv], [dirs[v] / 'ExeFS/main' for v in sv], sh / 'pkg', env, logs, g)   # its own stage
    except BuildError as e: bi.PROG.end(str(e), ok=False); say('[RED] the game is packed without shaders (its draws are skipped)'); return None
    bi.PROG.stage('shaders for Vulkan', step='translating')
    out = work / 'spv'
    try: bi.run([sys.executable, REPO / 'dm/android/tools/spv_build.py', sh / 'pkg', out, '--xlate', REPO / 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate.dll',
                 '--facts', g.table('shader_facts.tsv')], logs / 'spv_build.log', env=env)
    except BuildError as e: bi.PROG.end(str(e), ok=False); say(f'[RED] shader translation failed (see {logs / "spv_build.log"}): the game is packed with the shaders translated so far'); return out if out.is_dir() else None
    last = [l for l in (logs / 'spv_build.log').read_text(errors='replace').splitlines() if l.startswith('spv_build:')]
    bi.PROG.end(last[-1][11:] if last else str(out))
    return out

def pick_device():
    """The one device adb lists as ready ('device'): an offline Wi-Fi entry left from an earlier connection does not count"""
    out = subprocess.run(['adb', 'devices'], capture_output=True, text=True, errors='replace').stdout.splitlines()[1:]
    rows = [l.split() for l in out if l.strip()]
    ready = [r[0] for r in rows if len(r) > 1 and r[1] == 'device']
    if len(ready) == 1: return ready[0]
    listed = ', '.join(f'{r[0]} ({r[1]})' for r in rows) or 'none'
    raise BuildError(f'{len(ready)} adb devices ready ({listed}): connect one, or pick it with --serial')

def adb(serial, *a, log=None):
    cmd = ['adb'] + (['-s', serial] if serial else []) + [str(x) for x in a]
    r = subprocess.run(cmd, capture_output=True, text=True, errors='replace', env=dict(os.environ, MSYS_NO_PATHCONV='1'))
    if log: log.write_text(r.stdout + r.stderr)
    if r.returncode: raise BuildError(f'{" ".join(cmd)} failed: {(r.stdout + r.stderr).strip()[-400:]}')
    return r.stdout


def push_dir(serial, local, name, log, sync=False):
    # <local folder> → REMOTE/game/<name>: adb push copies a folder by its own name into the remote parent ("<folder>/." makes "/./" paths that adb cannot create
    # subfolders under on Windows); a folder named otherwise is pushed under its own name and renamed. adbd may not create folders under Android/data (its FUSE
    # mount refuses them: "secure_mkdirs() failed"), while the shell may: the whole tree is made first by a mkdir script run in the shell
    parent = f'{REMOTE}/game'
    dirs = [f'{parent}/{local.name}'] + [f'{parent}/{local.name}/{d.relative_to(local).as_posix()}' for d in sorted(local.rglob('*')) if d.is_dir()]
    script = log.with_suffix('.mkdirs.sh')
    script.write_text(''.join("mkdir -p '" + d.replace("'", "'\\''") + "'\n" for d in dirs), newline='\n')
    adb(serial, 'push', script, '/data/local/tmp/nxnative_mkdirs.sh'); adb(serial, 'shell', 'sh', '/data/local/tmp/nxnative_mkdirs.sh')
    adb(serial, 'push', *(['--sync'] if sync else []), local, f'{parent}/', log=log)
    if local.name != name:
        adb(serial, 'shell', 'rm', '-rf', f'{parent}/{name}'); adb(serial, 'shell', 'mv', f'{parent}/{local.name}', f'{parent}/{name}')


def main(argv):
    ap = argparse.ArgumentParser(description='Pack a Switch game you own for Android (APK + game files on the phone)')
    ap.add_argument('nsp_dir'); ap.add_argument('-k', '--keys', required=True, help='your prod.keys')
    ap.add_argument('-p', '--package', help='the version to pack (v0 = the base game; default: the newest in the folder)')
    ap.add_argument('-l', '--language', default='en-US', help='the game language (a code or a name; English by default)')
    ap.add_argument('--sdk', help='the Android SDK (default ANDROID_HOME)'); ap.add_argument('--serial', help='the adb device (default: the only one)')
    ap.add_argument('--no-push', action='store_true', help='build the APK only; do not install it or copy the game files to the phone')
    ap.add_argument('--allow-damaged', action='store_true', help='pack files that fail the NCA hash check as read')
    a = ap.parse_args(argv)
    bi.PROG.mode = bi.progress_mode('auto'); bi.ALLOW_DAMAGED = a.allow_damaged
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
    if g is None: raise BuildError(f'no game profile for {app:016X}: the Android build packs games with a profile in dm/games (see README.md for the list)')
    say(f'game profile: {g.id} ({g.get("name", "")})')
    builds = g.builds(); have = bi.nsp_versions(fam, builds)
    pkg_ver = a.package or max(have, key=bi.vnum)
    if pkg_ver not in have: raise BuildError(f'version {pkg_ver} is not in the folder, which holds {", ".join(sorted(have, key=bi.vnum))}')
    # the shaders are split from every version the profile's shader tables were made from (shader_versions), as the iOS build does
    sv = g.words('shader_versions') or [pkg_ver]
    if not all(v in have for v in sv): say(f'[RED] the folder has no {", ".join(v for v in sv if v not in have)}, whose files the profile shader tables were made from: no shaders are translated'); sv = []
    picks = bi.pick_nsps(fam, have, sorted(set(sv) | {pkg_ver}, key=bi.vnum))
    work = nsp_dir / f'{g.id}_build'; logs = work / 'logs'; ex = work / 'extract'
    ex.mkdir(parents=True, exist_ok=True); (work / 'tmp').mkdir(exist_ok=True)
    env.update(DM_GAME=g.id, DM_ROOT=str(work), WORK=str(work / 'tmp'), DM_TARGET='android', PYTHONUTF8='1')   # the tools' text files in UTF-8 on Windows too (its default is the ANSI code page)
    bi.PROG.plan(5 + 2 * bool(sv) + len(set(sv) - {pkg_ver}) + (not a.no_push))   # an extraction per other shader version
    bi.PROG.stage('build nx_extract'); nx = build_nx_extract(work, logs, env); bi.PROG.end()
    b, u = picks[pkg_ver]
    game = bi.extract(nx, keys, ex, pkg_ver, b, u, None, logs, g)
    bad = bi.build_mismatch(game, pkg_ver, g)
    if bad: say(f'[RED] not the builds game profile {g.id} registers ({"; ".join(bad)}): the per-build fixes of the profile do not reach these modules, the game may not boot')
    have_l = nacp.languages(game / 'control.nacp')
    lang = bi.game_language(bi.language_code(a.language) or 'en-US', a.language != 'en-US', have_l)
    patched = work / f'patched_android_{pkg_ver}'
    g = bi.rewrite(sys.executable, REPO, game / 'ExeFS', patched, env, logs, g)
    spv = build_shaders(sv, {v: (game if v == pkg_ver else bi.extract(nx, keys, ex, v, picks[v][0], picks[v][1], None, logs, g)) for v in sv}, work, logs, env, g)
    bi.PROG.stage('Android host'); so, ndk = build_host(sdk, g.id, work, logs, env); st = build_selftest(ndk, work, logs, env); bi.PROG.end(str(so))
    apk = nsp_dir / (Path(g.get('ipa') or f'{g.id}.ipa').stem + '.apk')
    bi.PROG.stage('APK')
    KEYSTORE.parent.mkdir(parents=True, exist_ok=True)
    bi.run([sys.executable, REPO / 'dm/android/tools/build_apk.py', '--sdk', sdk, '--so', so, '--selftest', st, '--keystore', KEYSTORE, '-o', apk], logs / 'android_apk.log', env=env)
    font = button_font(work, logs, env)
    bi.PROG.end(str(apk))
    if a.no_push:
        rom = bi.rfs.source(game)
        say(f'done: {apk}. On the phone: install it, then copy {patched} to {REMOTE}/game/code and {rom} to {REMOTE}/game/{rom.name}'); return
    bi.PROG.stage('install and copy to the phone')
    a.serial = a.serial or os.environ.get('ANDROID_SERIAL') or pick_device()
    try: adb(a.serial, 'install', '-r', apk, log=logs / 'adb_install.log')
    except BuildError as e:
        if 'INSTALL_FAILED_UPDATE_INCOMPATIBLE' in str(e):
            raise BuildError(f'the nxnative app on the phone was signed with another key than {KEYSTORE}. Uninstalling it deletes its saves and game files: '
                             f'back up {REMOTE} and the saves first, then uninstall it by hand and run this again') from e
        raise
    adb(a.serial, 'shell', 'am', 'force-stop', PKG)
    adb(a.serial, 'shell', 'rm', '-rf', f'{REMOTE}/game/code', f'{REMOTE}/game/font', f'{REMOTE}/game/{patched.name}'); adb(a.serial, 'shell', 'mkdir', '-p', f'{REMOTE}/game')
    push_dir(a.serial, font, 'font', logs / 'adb_push_font.log')
    push_dir(a.serial, patched, 'code', logs / 'adb_push_code.log')
    if spv: push_dir(a.serial, spv, 'spv', logs / 'adb_push_spv.log', sync=True)
    bi.PROG.set(step='RomFS (this takes a while for a large game)')
    rom = bi.rfs.source(game)   # the extraction's RomFS image (RomFS.img: the host reads it as it is, hle_fs.c), or a RomFS folder from an older extraction
    if rom.is_file():
        adb(a.serial, 'shell', 'rm', '-rf', f'{REMOTE}/game/RomFS')
        adb(a.serial, 'push', '--sync', rom, f'{REMOTE}/game/RomFS.img', log=logs / 'adb_push_romfs.log')
    else:
        adb(a.serial, 'shell', 'rm', '-f', f'{REMOTE}/game/RomFS.img')
        push_dir(a.serial, rom, 'RomFS', logs / 'adb_push_romfs.log', sync=True)   # --sync: a repeat push copies only what changed
    aoc = work / 'aoc'
    if aoc.is_dir(): push_dir(a.serial, aoc, 'aoc', logs / 'adb_push_aoc.log', sync=True)
    # the settings screen (dm/android/java Launcher.java): the game's icon and control data (name, languages), and its defaults (the language picked with --language);
    # env.txt is the player's and stays as it is (a key it sets wins over the settings screen)
    for f in ('icon.jpg', 'control.nacp'):
        if (game / f).is_file(): adb(a.serial, 'push', game / f, f'{REMOTE}/game/{f}')
    dflt = work / 'android_defaults.txt'; dflt.write_text(f'DM_LANG={lang}\n')
    adb(a.serial, 'push', dflt, f'{REMOTE}/game/defaults.txt')
    bi.PROG.end(f'{REMOTE}/game')
    say(f'done: open "nxnative" on the phone. Log: {REMOTE}/log.txt (adb logcat -s nxnative)')


if __name__ == '__main__':
    try: main(sys.argv[1:])
    except BuildError as e:
        bi.PROG.close(ok=False); bi.PROG.say(f'[RED] {e}'); sys.exit(1)
    finally: bi.PROG.close(ok=False)
