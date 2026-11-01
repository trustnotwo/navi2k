"""Numbered builds of Navi2K Player.

    python -I scripts/make_build.py              next dev build:  Dev/<x.y>-dev-NNNN/
    python -I scripts/make_build.py --release    release build:   Releases/<x.y>/

Each build gets a folder under Dev/ or Releases/ in F:/Navi2K-dev, with two subfolders:
  source/  the exact source it was built from (version.h stamped with its number)
  bin/     the stripped .exes, the East Asian fallback font (package/fonts, used on 9x only),
           README.txt, CHANGELOG.txt and licenses
A release also gets Navi2K-<x.y>-win32.zip of bin/ (never the ini or log).

Dev build numbers are four digits, counted across every Dev/*-dev-NNNN folder. The base
version (x.y) comes from src/version.h. The toolchain is looked up in NAVI2K_TOOLS, or
%LOCALAPPDATA%/Navi2K-tools (cmake-4.4.4-windows-x86_64 and mingw32 inside).
"""
import glob
import os
import re
import shutil
import subprocess
import sys
import zipfile

TREE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# The folder holding Dev/, Releases/ and dist/: the source tree itself, or its parent when
# the tree is a subfolder (as the earlier Navi2K-mmx tree was).
ROOT = TREE if os.path.isdir(os.path.join(TREE, 'Releases')) else os.path.dirname(TREE)
BUILD = os.path.join(TREE, 'build')
VERSION_H = os.path.join(TREE, 'src', 'version.h')
SOURCE_ITEMS = ['CMakeLists.txt', 'README.md', 'CONFIGURATION.md', 'CHANGELOG.md', 'docs', 'cmake', 'src', 'third_party', 'scripts', 'package']
CMAKE_FLAGS = ['-G', 'MinGW Makefiles', '-DCMAKE_BUILD_TYPE=Release', '-DLEGACY_MARCH=i586', '-DLEGACY_NO_SSE2=ON',
               '-DLEGACY_SSE2_DISPATCH=ON', '-DLEGACY_STRICT_CHECK=ON', '-DLIBMAD_FPM=FPM_INTEL']
EXES = ['Navi2KPlayer.exe', 'navi2k_probe.exe']


def fail(msg):
    print('ERROR: ' + msg)
    sys.exit(1)


def base_version():
    text = open(VERSION_H, encoding='utf-8').read()
    nums = [int(re.search(r'#define LP_VERSION_%s (\d+)' % k, text).group(1)) for k in ('MAJOR', 'MINOR', 'PATCH')]
    return nums, ('%d.%d' % (nums[0], nums[1]) if nums[2] == 0 else '%d.%d.%d' % tuple(nums))


def next_dev_number():
    numbers = [0]
    for path in glob.glob(os.path.join(ROOT, 'Dev', '*-dev-[0-9][0-9][0-9][0-9]')):
        numbers.append(int(path[-4:]))
    return max(numbers) + 1


def stamp_version(build, text):
    t = open(VERSION_H, encoding='utf-8', newline='').read()
    t = re.sub(r'#define LP_DEV_BUILD \d+', '#define LP_DEV_BUILD %d' % build, t)
    t = re.sub(r'#define LP_VERSION_TEXT "[^"]*"', '#define LP_VERSION_TEXT "%s"' % text, t)
    open(VERSION_H, 'w', encoding='utf-8', newline='').write(t)


def tool_path():
    tools = os.environ.get('NAVI2K_TOOLS') or os.path.join(os.environ['LOCALAPPDATA'], 'Navi2K-tools')
    bins = [os.path.join(tools, 'mingw32', 'bin'), os.path.join(tools, 'cmake-4.4.4-windows-x86_64', 'bin')]
    for b in bins:
        if not os.path.isdir(b):
            fail('toolchain folder not found: ' + b)
    env = dict(os.environ)
    env['PATH'] = os.pathsep.join(bins + [env.get('PATH', '')])
    return env


def run(cmd, env):
    exe = shutil.which(cmd[0], path=env['PATH'])   # Windows searches the caller's PATH, not env's
    if not exe:
        fail(cmd[0] + ' not found in the toolchain')
    cmd = [exe] + cmd[1:]
    r = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
    if r.returncode != 0:
        print(r.stdout[-6000:])
        fail('command failed: ' + ' '.join(cmd))
    return r.stdout


def private_values():
    """The server address, token and salt from the developer's own ini (dist/Navi2KPlayer.ini),
    read at build time so they never have to be written into this file."""
    values = []
    ini = os.path.join(ROOT, 'dist', 'Navi2KPlayer.ini')
    if os.path.exists(ini):
        raw = open(ini, 'rb').read()
        text = raw.decode('utf-16') if raw[:2] in (b'\xff\xfe', b'\xfe\xff') else raw.decode('latin-1')
        for line in text.splitlines():
            key, _, value = line.partition('=')
            value = value.strip()
            if key.strip() in ('Token', 'Salt') and len(value) >= 8:
                values.append(value)
            elif key.strip() in ('Url', 'LanUrl') and '://' in value:
                host = value.split('://', 1)[1].split('/')[0].split(':')[0]
                if len(host) >= 4:
                    values.append(host)
    return values


def privacy_scan(folder):
    """Login keys and the developer's own server details must never ship."""
    bad = [re.compile(b'(?<!publicKey)' + b'Tok' + b'en='), re.compile(b'Sa' + b'lt=')]
    for v in private_values():
        bad.append(re.compile(re.escape(v.encode('utf-8'))))
        bad.append(re.compile(re.escape(v.encode('utf-16-le'))))
    hits = []
    for dirpath, _, names in os.walk(folder):
        for name in names:
            if name.lower().endswith(('.ini', '.log')):
                hits.append(os.path.join(dirpath, name) + ' (ini/log file)')
                continue
            data = open(os.path.join(dirpath, name), 'rb').read()
            if any(p.search(data) for p in bad):
                hits.append(os.path.join(dirpath, name))
    return hits


def main():
    release = '--release' in sys.argv[1:]
    nums, base = base_version()
    if release:
        build, text, out = 0, base, os.path.join(ROOT, 'Releases', base)
    else:
        build = next_dev_number()
        text = '%s-dev-%04d' % (base, build)
        out = os.path.join(ROOT, 'Dev', text)
    if os.path.exists(out):
        fail(out + ' already exists')

    print('Building ' + text)
    stamp_version(build, text)
    env = tool_path()
    if not os.path.exists(os.path.join(BUILD, 'CMakeCache.txt')):
        run(['cmake', '-S', TREE, '-B', BUILD] + CMAKE_FLAGS, env)
    run(['cmake', '--build', BUILD], env)   # fails here if the Win9x/2000/Pentium checks do
    if not all(os.path.exists(os.path.join(BUILD, e)) for e in EXES):
        fail('build did not produce both executables')
    for e in EXES:
        if text.encode('utf-16-le') not in open(os.path.join(BUILD, e), 'rb').read():
            fail(e + ' does not carry version ' + text + ' (stale build?)')

    src_out, bin_out = os.path.join(out, 'source'), os.path.join(out, 'bin')
    os.makedirs(src_out)
    os.makedirs(bin_out)
    for item in SOURCE_ITEMS:
        p = os.path.join(TREE, item)
        if os.path.isdir(p):
            shutil.copytree(p, os.path.join(src_out, item), ignore=shutil.ignore_patterns('__pycache__'))
        elif os.path.exists(p):
            shutil.copy2(p, src_out)
    for e in EXES:
        run(['strip', '-s', '-o', os.path.join(bin_out, e), os.path.join(BUILD, e)], env)
    shutil.copytree(os.path.join(TREE, 'package', 'licenses'), os.path.join(bin_out, 'licenses'))
    shutil.copy2(os.path.join(TREE, 'package', 'CHANGELOG.txt'), bin_out)
    for font in glob.glob(os.path.join(TREE, 'package', 'fonts', '*.ttf')):   # loaded on 9x, next to the exe
        shutil.copy2(font, bin_out)
    readme = open(os.path.join(TREE, 'package', 'README.txt'), encoding='utf-8', newline='').read()
    nl = '\r\n' if '\r\n' in readme else '\n'
    lines = readme.split(nl)
    lines[0] = 'Navi2K Player ' + text
    lines[1] = '=' * len(lines[0])
    open(os.path.join(bin_out, 'README.txt'), 'w', encoding='utf-8', newline='').write(nl.join(lines))

    hits = privacy_scan(out)
    if hits:
        shutil.rmtree(out)
        fail('private data found, build folder removed: ' + ', '.join(hits))

    if release:
        top = 'Navi2K-Player-%s-win32' % text
        zpath = os.path.join(out, 'Navi2K-%s-win32.zip' % text)
        with zipfile.ZipFile(zpath, 'w', zipfile.ZIP_DEFLATED) as z:
            for dirpath, _, names in os.walk(bin_out):
                for name in sorted(names):
                    full = os.path.join(dirpath, name)
                    rel = os.path.relpath(full, bin_out).replace(os.sep, '/')
                    z.write(full, top + '/' + rel)
        print('Zip: ' + zpath)
    print('Done: ' + out)


if __name__ == '__main__':
    main()
