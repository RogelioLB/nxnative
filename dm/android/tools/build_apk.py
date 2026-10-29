# Packs the Android app (no Gradle): the manifest linked by aapt2, lib/arm64-v8a/ (the loader libnxloader.so and a host per game, libdmhost_<title ID>.so), classes.dex
# from dm/android/java (javac, then d8), the self test in assets/selftest (the synthetic module of dm/android/tests/selftest_nso.py, rewritten with DM_TARGET=android),
# page-aligned by zipalign, signed by apksigner.
# Usage: build_apk.py --sdk <Android SDK> --loader <libnxloader.so> --so <libdmhost_<id>.so> [--so ...] --selftest <folder of main.patched.bin, main.fixups.tsv, ...>
#   [--demo <folder>] --keystore <.jks> -o <out.apk> [--label <app name>] [--icon <icon.jpg or .png>] [--build-tools 36.0.0] [--platform android-35]
# The app is "nxnative" with the adaptive icon in apk/res: its launcher shows the games' own icons and names. --label and --icon name it otherwise.
# The keystore is made on first use (keytool, a debug key: alias nxnative, password "android"); keep it outside the repository.
import argparse, os, shutil, subprocess, sys, tempfile, zipfile
from xml.sax.saxutils import quoteattr

HERE = os.path.dirname(os.path.abspath(__file__))

def run(*a):
    r = subprocess.run(a, capture_output=True, text=True, errors='replace')
    if r.returncode: sys.exit(f'{a[0]} failed ({r.returncode}):\n{r.stdout}{r.stderr}')

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--sdk', required=True); ap.add_argument('--selftest', required=True)
    ap.add_argument('--so', action='append', required=True, help='a game host, libdmhost_<title ID>.so (once per game)'); ap.add_argument('--loader', required=True, help='libnxloader.so')
    ap.add_argument('--demo', help='folder for assets/demo: the synthetic shaders and their SPIR-V (tests/synth_shaders.py, tools/spv_android.py)')
    ap.add_argument('--keystore', required=True); ap.add_argument('-o', '--out', required=True)
    ap.add_argument('--label', help="the app's name in the launcher (default nxnative)"); ap.add_argument('--icon', help="the app's launcher icon: a JPEG or PNG (the game's icon.jpg)")
    ap.add_argument('--build-tools', default='36.0.0'); ap.add_argument('--platform', default='android-35')
    a = ap.parse_args()
    for so in a.so:
        n = os.path.basename(so)
        if not (n.startswith('libdmhost_') and n.endswith('.so')): sys.exit(f'--so must be a game host named libdmhost_<title ID>.so: {so}')
    bt = os.path.join(a.sdk, 'build-tools', a.build_tools); exe = '.exe' if os.name == 'nt' else ''; bat = '.bat' if os.name == 'nt' else ''
    jar = os.path.join(a.sdk, 'platforms', a.platform, 'android.jar')
    if not os.path.exists(a.keystore):
        run('keytool', '-genkeypair', '-keystore', a.keystore, '-storepass', 'android', '-keypass', 'android', '-alias', 'nxnative',
            '-keyalg', 'RSA', '-keysize', '2048', '-validity', '10000', '-dname', 'CN=nxnative debug')
    with tempfile.TemporaryDirectory() as d:
        base = os.path.join(d, 'base.apk'); aapt2 = os.path.join(bt, 'aapt2' + exe)
        # the manifest with the app's label and icon: the application element's label replaced, an icon attribute added beside it (a drawable compiled by aapt2)
        with open(os.path.join(HERE, '..', 'apk', 'AndroidManifest.xml'), encoding='utf-8') as f: manifest = f.read()
        # the label always: the application element's is replaced whole, and an app with none shows its package name (org.nxnative.android)
        attrs, res = f' android:label={quoteattr(a.label or "nxnative")}', []
        if a.icon:
            ext = os.path.splitext(a.icon)[1].lower()
            if ext not in ('.jpg', '.jpeg', '.png'): sys.exit(f'--icon must be a JPEG or PNG: {a.icon}')
            rd = os.path.join(d, 'res', 'drawable'); os.makedirs(rd); shutil.copyfile(a.icon, os.path.join(rd, 'icon' + ('.jpg' if ext == '.jpeg' else ext)))
            run(aapt2, 'compile', '--dir', os.path.join(d, 'res'), '-o', os.path.join(d, 'res.zip')); res = ['-R', os.path.join(d, 'res.zip'), '--auto-add-overlay']
            attrs += ' android:icon="@drawable/icon" android:roundIcon="@drawable/icon"'
        else:   # the app's own adaptive icon (apk/res: the mark on ink, vector drawables, with a monochrome layer for themed icons)
            run(aapt2, 'compile', '--dir', os.path.join(HERE, '..', 'apk', 'res'), '-o', os.path.join(d, 'res.zip')); res = ['-R', os.path.join(d, 'res.zip'), '--auto-add-overlay']
            attrs += ' android:icon="@mipmap/ic_launcher" android:roundIcon="@mipmap/ic_launcher"'
        if attrs:
            if '<application android:label="nxnative"' not in manifest: sys.exit('AndroidManifest.xml: no <application android:label="nxnative"> to name the app in')
            manifest = manifest.replace('<application android:label="nxnative"', '<application' + attrs, 1)
        mf = os.path.join(d, 'AndroidManifest.xml')
        with open(mf, 'w', encoding='utf-8') as f: f.write(manifest)
        run(aapt2, 'link', '-o', base, '--manifest', mf, '-I', jar, *res, '--min-sdk-version', '29', '--target-sdk-version', '34')
        # the Java side (the touch controls, the text box): javac for Java 8 against the platform's android.jar, then d8 to classes.dex
        src = os.path.join(HERE, '..', 'java'); cls = os.path.join(d, 'classes'); os.makedirs(cls)
        javas = [os.path.join(r, f) for r, _, fs in os.walk(src) for f in fs if f.endswith('.java')]
        run('javac', '-source', '8', '-target', '8', '-Xlint:-options', '-encoding', 'UTF-8', '-bootclasspath', jar, '-d', cls, *javas)
        run(os.path.join(bt, 'd8' + bat), '--lib', jar, '--min-api', '29', '--output', d, *[os.path.join(r, f) for r, _, fs in os.walk(cls) for f in fs if f.endswith('.class')])
        with zipfile.ZipFile(base, 'a', zipfile.ZIP_DEFLATED) as z:
            z.write(os.path.join(d, 'classes.dex'), 'classes.dex')
            z.write(a.loader, 'lib/arm64-v8a/libnxloader.so')
            for so in sorted(set(a.so)): z.write(so, f'lib/arm64-v8a/{os.path.basename(so)}')
            n = 0
            for f in sorted(os.listdir(a.selftest)):
                if f.startswith('main.'): z.write(os.path.join(a.selftest, f), f'assets/selftest/{f}'); n += 1
            if not n: sys.exit(f'no self test module (main.*) in {a.selftest}')
            for f in sorted(os.listdir(a.demo)) if a.demo else []: z.write(os.path.join(a.demo, f), f'assets/demo/{f}')
        aligned = os.path.join(d, 'aligned.apk')
        run(os.path.join(bt, 'zipalign' + exe), '-f', '-P', '16', '4', base, aligned)
        run(os.path.join(bt, 'apksigner' + bat), 'sign', '--ks', a.keystore, '--ks-pass', 'pass:android', '--key-pass', 'pass:android', '--out', a.out, aligned)
    print(f'{a.out}: {os.path.getsize(a.out):,} bytes')

if __name__ == '__main__':
    main()
