"""FAT12 floppy images of a build's bin folder.

    python -I scripts/make_floppy.py <build folder>          one 2.88 MB disk with everything
    python -I scripts/make_floppy.py <build folder> --1440   two 1.44 MB disks

e.g. <build folder> = F:/Navi2K-dev/Dev/1.3-dev-0002. A 2.88 MB disk holds the whole build;
on 1.44 MB disks the player (with README, CHANGELOG and licenses) and the test tool go on
separate disks, since together they are too big for one. Long file names are stored as
VFAT entries (Windows 95 and later show them; DOS sees 8.3 names). The ini, logs and any
images or zips in bin/ are never copied.
"""
import os
import random
import struct
import sys
import time

SECTOR = 512
RESERVED = 1
FATS = 2
SKIP = ('.ini', '.log', '.img', '.iso', '.zip')
SHORT_OK = set('ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!#$%&\'()-@^_`{}~')

# The standard DOS formats: total sectors, sectors per cluster, root entries, sectors per
# FAT, sectors per track (2 heads, media byte F0 for both).
FORMATS = {
    1440: (2880, 1, 224, 9, 18),
    2880: (5760, 2, 240, 9, 36),
}


class Format:
    def __init__(self, kb):
        self.total, self.spc, self.root_entries, self.fat_sectors, self.spt = FORMATS[kb]
        self.root_sectors = self.root_entries * 32 // SECTOR
        self.data_start = RESERVED + FATS * self.fat_sectors + self.root_sectors
        self.clusters = (self.total - self.data_start) // self.spc
        self.cluster_bytes = self.spc * SECTOR


def dos_datetime(t):
    lt = time.localtime(t)
    d = ((max(lt.tm_year, 1980) - 1980) << 9) | (lt.tm_mon << 5) | lt.tm_mday
    tm = (lt.tm_hour << 11) | (lt.tm_min << 5) | (lt.tm_sec // 2)
    return d, tm


def short_name(name, taken):
    """An 8.3 name (11 bytes, space padded) and whether the long name must be stored too."""
    base, dot, ext = name.rpartition('.') if '.' in name.lstrip('.') else (name, '', '')
    clean = lambda s: ''.join(c if c in SHORT_OK else '_' for c in s.upper().replace(' ', ''))
    b, e = clean(base), clean(ext)[:3]
    exact = b == base and e == ext and len(b) <= 8 and len(ext) <= 3
    if exact and (b.ljust(8) + e.ljust(3)) not in taken:
        return (b.ljust(8) + e.ljust(3)).encode('ascii'), False
    for n in range(1, 100):
        tail = '~%d' % n
        cand = (b[:8 - len(tail)] + tail).ljust(8) + e.ljust(3)
        if cand not in taken:
            return cand.encode('ascii'), True
    raise ValueError('too many similar names: ' + name)


def lfn_checksum(short):
    s = 0
    for c in short:
        s = (((s & 1) << 7) + (s >> 1) + c) & 0xFF
    return s


def lfn_entries(name, short):
    units = name.encode('utf-16-le')
    chars = [units[i:i + 2] for i in range(0, len(units), 2)] + [b'\x00\x00']
    while len(chars) % 13:
        chars.append(b'\xff\xff')
    parts = [chars[i:i + 13] for i in range(0, len(chars), 13)]
    csum = lfn_checksum(short)
    out = []
    for seq in range(len(parts), 0, -1):     # stored last part first
        p = parts[seq - 1]
        order = seq | (0x40 if seq == len(parts) else 0)
        out.append(bytes([order]) + b''.join(p[0:5]) + bytes([0x0F, 0, csum]) + b''.join(p[5:11]) + b'\0\0' +
                   b''.join(p[11:13]))
    return out


def dir_entry(short, attr, cluster, size, mtime):
    d, t = dos_datetime(mtime)
    return struct.pack('<11sBBBHHHHHHHI', short, attr, 0, 0, t, d, d, 0, t, d, cluster, size)


class Floppy:
    def __init__(self, fmt, label):
        self.f = fmt
        self.label = label
        self.fat = [0xFF0, 0xFFF] + [0] * fmt.clusters
        self.data = {}                        # cluster -> bytes
        self.next = 2

    def alloc(self, payload):
        """Stores payload in a cluster chain; returns the first cluster (0 if empty)."""
        cb = self.f.cluster_bytes
        n = (len(payload) + cb - 1) // cb
        if n == 0:
            return 0
        if self.next + n > self.f.clusters + 2:
            raise ValueError('disk full')
        first = self.next
        for i in range(n):
            c = first + i
            self.fat[c] = c + 1 if i < n - 1 else 0xFFF
            self.data[c] = payload[i * cb:(i + 1) * cb].ljust(cb, b'\0')
        self.next += n
        return first

    def add(self, entries, taken, path, parent_cluster):
        name = os.path.basename(path)
        short, need_lfn = short_name(name, taken)
        taken.add(short.decode('ascii'))
        if need_lfn:
            entries.extend(lfn_entries(name, short))
        mtime = os.path.getmtime(path)
        if os.path.isdir(path):
            entries.append(self.subdir(path, short, mtime, parent_cluster))
        else:
            payload = open(path, 'rb').read()
            entries.append(dir_entry(short, 0x20, self.alloc(payload), len(payload), mtime))

    def subdir(self, path, short, mtime, parent_cluster):
        # Reserve the directory's clusters first so '.' can point at them.
        names = [n for n in sorted(os.listdir(path), key=str.lower)
                 if not (os.path.isfile(os.path.join(path, n)) and n.lower().endswith(SKIP))]
        cb = self.f.cluster_bytes
        n = ((len(names) * 4 + 2) * 32 + cb - 1) // cb
        first = self.alloc(b'\0' * n * cb)
        now = time.time()
        entries = [dir_entry(b'.          ', 0x10, first, 0, now), dir_entry(b'..         ', 0x10, parent_cluster, 0, now)]
        taken = set()
        for name in names:
            self.add(entries, taken, os.path.join(path, name), first)
        body = b''.join(entries)
        if len(body) > n * cb:
            raise ValueError('directory too big: ' + path)
        body = body.ljust(n * cb, b'\0')
        for i in range(n):
            self.data[first + i] = body[i * cb:(i + 1) * cb]
        return dir_entry(short, 0x10, first, 0, mtime)

    def build(self, paths):
        root = [dir_entry(self.label.ljust(11)[:11].encode('ascii'), 0x08, 0, 0, time.time())]
        taken = set()
        for path in paths:
            self.add(root, taken, path, 0)
        if len(root) > self.f.root_entries:
            raise ValueError('root directory full')
        return self.image(b''.join(root).ljust(self.f.root_sectors * SECTOR, b'\0'))

    def image(self, root):
        f = self.f
        img = bytearray(f.total * SECTOR)
        img[0:SECTOR] = boot_sector(f, self.label)
        fat = bytearray(f.fat_sectors * SECTOR)
        entries = self.fat + [0] * (len(self.fat) % 2)
        for i in range(0, len(entries), 2):
            a, b = entries[i], entries[i + 1]
            o = i * 3 // 2
            fat[o:o + 3] = bytes([a & 0xFF, ((a >> 8) & 0x0F) | ((b & 0x0F) << 4), (b >> 4) & 0xFF])
        for k in range(FATS):
            o = (RESERVED + k * f.fat_sectors) * SECTOR
            img[o:o + len(fat)] = fat
        o = (RESERVED + FATS * f.fat_sectors) * SECTOR
        img[o:o + len(root)] = root
        for c, chunk in self.data.items():
            o = (f.data_start + (c - 2) * f.spc) * SECTOR
            img[o:o + len(chunk)] = chunk
        return bytes(img)


def boot_sector(f, label):
    """The format's BPB and a few bytes of code that say the disk isn't bootable."""
    bs = bytearray(SECTOR)
    bs[0:3] = b'\xEB\x3C\x90'
    bs[3:11] = b'MSWIN4.1'
    struct.pack_into('<HBHBHHBHHHII', bs, 11, SECTOR, f.spc, RESERVED, FATS, f.root_entries, f.total, 0xF0,
                     f.fat_sectors, f.spt, 2, 0, 0)
    struct.pack_into('<BBBI11s8s', bs, 36, 0, 0, 0x29, random.getrandbits(32), label.ljust(11)[:11].encode('ascii'),
                     b'FAT12   ')
    msg = b'\r\nThis disk is not bootable. Press a key to restart.\r\n\0'
    code = bytearray()
    code += b'\xFA\x31\xC0\x8E\xD8'          # cli; xor ax,ax; mov ds,ax
    si_at = len(code) + 1
    code += b'\xBE\x00\x00\xFB'              # mov si,msg; sti
    loop = len(code)
    code += b'\xAC\x08\xC0'                  # lodsb; or al,al
    jz_at = len(code)
    code += b'\x74\x00'                      # jz done
    code += b'\xB4\x0E\xBB\x07\x00\xCD\x10'  # mov ah,0Eh; mov bx,7; int 10h
    code += b'\xEB' + struct.pack('b', loop - (len(code) + 2))
    done = len(code)
    code += b'\x31\xC0\xCD\x16\xCD\x19'      # xor ax,ax; int 16h; int 19h
    code[jz_at + 1] = done - (jz_at + 2)
    struct.pack_into('<H', code, si_at, 0x7C00 + 0x3E + len(code))
    code += msg
    bs[0x3E:0x3E + len(code)] = code
    bs[510:512] = b'\x55\xAA'
    return bytes(bs)


def main():
    out = os.path.abspath(sys.argv[1])
    name = os.path.basename(out)
    bin_dir = os.path.join(out, 'bin')
    label = 'NAVI2K ' + name[-4:]
    everything = ['Navi2KPlayer.exe', 'navi2k_probe.exe', 'README.txt', 'CHANGELOG.txt', 'licenses']
    if '--1440' in sys.argv[2:]:
        disks = [(1440, 'disk1', label, ['Navi2KPlayer.exe', 'README.txt', 'CHANGELOG.txt', 'licenses']),
                 (1440, 'disk2', 'N2K TOOLS', ['navi2k_probe.exe', 'README.txt'])]
    else:
        disks = [(2880, '2.88MB', label, everything)]
    for kb, suffix, lab, items in disks:
        img = Floppy(Format(kb), lab).build([os.path.join(bin_dir, i) for i in items])
        path = os.path.join(out, 'Navi2K-%s-%s.img' % (name, suffix))
        open(path, 'wb').write(img)
        print(path)


if __name__ == '__main__':
    main()
