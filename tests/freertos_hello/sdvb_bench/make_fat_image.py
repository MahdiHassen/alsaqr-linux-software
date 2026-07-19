#!/usr/bin/env python3
"""
Build a bare FAT16 filesystem image (BPB at offset 0, no MBR) containing a
DISP/ subdirectory with the disparity stereo pair. Layout matches what
fat16.c / fat_file.c expect (depth-1 subdir resolution).

Usage: make_fat_image.py <src_data_dir> <out.img>
  <src_data_dir> must contain 1.bmp and 2.bmp (e.g. the QCIF dataset).
"""
import struct, sys, os

BPS = 512
SPC = 1
RESERVED = 1
NFATS = 2
ROOT_ENTRIES = 512
N_CLUSTERS = 8192          # >= 4085 forces FAT16 (not FAT12)

def ceil_div(a, b): return (a + b - 1) // b

def main():
    src, out = sys.argv[1], sys.argv[2]
    data1 = open(os.path.join(src, "1.bmp"), "rb").read()
    data2 = open(os.path.join(src, "2.bmp"), "rb").read()

    spf = ceil_div((N_CLUSTERS + 2) * 2, BPS)
    root_sectors = ceil_div(ROOT_ENTRIES * 32, BPS)
    data_start = RESERVED + NFATS * spf + root_sectors
    total_sectors = data_start + N_CLUSTERS * SPC
    CLUSTER_BYTES = SPC * BPS

    img = bytearray(total_sectors * BPS)

    # ---- boot sector / BPB ----
    img[0:3] = b'\xEB\x3C\x90'
    img[3:11] = b'MSWIN4.1'
    struct.pack_into('<H', img, 0x0B, BPS)
    img[0x0D] = SPC
    struct.pack_into('<H', img, 0x0E, RESERVED)
    img[0x10] = NFATS
    struct.pack_into('<H', img, 0x11, ROOT_ENTRIES)
    struct.pack_into('<H', img, 0x13, total_sectors if total_sectors < 0x10000 else 0)
    img[0x15] = 0xF8
    struct.pack_into('<H', img, 0x16, spf)
    struct.pack_into('<H', img, 0x18, 63)
    struct.pack_into('<H', img, 0x1A, 255)
    struct.pack_into('<I', img, 0x20, total_sectors if total_sectors >= 0x10000 else 0)
    img[0x24] = 0x80
    img[0x26] = 0x29
    struct.pack_into('<I', img, 0x27, 0x12345678)
    img[0x2B:0x36] = b'NO NAME    '
    img[0x36:0x3E] = b'FAT16   '
    img[510] = 0x55
    img[511] = 0xAA

    FAT1_OFF = RESERVED * BPS
    FAT2_OFF = (RESERVED + spf) * BPS
    ROOT_OFF = (RESERVED + NFATS * spf) * BPS
    DATA_OFF = data_start * BPS

    def set_fat(cluster, val):
        for fi in range(NFATS):
            base = FAT1_OFF if fi == 0 else FAT2_OFF
            struct.pack_into('<H', img, base + cluster * 2, val & 0xFFFF)

    set_fat(0, 0xFFF8)
    set_fat(1, 0xFFFF)

    def cluster_off(c): return DATA_OFF + (c - 2) * CLUSTER_BYTES

    next_free = [2]
    def alloc_chain(nbytes):
        n = max(1, ceil_div(nbytes, CLUSTER_BYTES))
        clusters = [next_free[0] + i for i in range(n)]
        next_free[0] += n
        for i in range(n):
            set_fat(clusters[i], 0xFFFF if i == n - 1 else clusters[i + 1])
        return clusters

    def write_clusters(clusters, data):
        off = 0
        for c in clusters:
            chunk = data[off:off + CLUSTER_BYTES]
            img[cluster_off(c):cluster_off(c) + len(chunk)] = chunk
            off += CLUSTER_BYTES

    def name83(name):
        base, _, ext = name.partition('.')
        return ((base.upper() + ' ' * 8)[:8] + (ext.upper() + ' ' * 3)[:3]).encode('ascii')

    def entry(name11, attr, cluster, size):
        e = bytearray(32)
        e[0:11] = name11
        e[11] = attr
        struct.pack_into('<H', e, 20, (cluster >> 16) & 0xFFFF)
        struct.pack_into('<H', e, 26, cluster & 0xFFFF)
        struct.pack_into('<I', e, 28, size)
        return e

    # ---- DISP subdirectory ----
    disp_clusters = alloc_chain(CLUSTER_BYTES)
    disp_cluster = disp_clusters[0]

    dir_blob = bytearray()
    dir_blob += entry(b'.' + b' ' * 10, 0x10, disp_cluster, 0)
    dir_blob += entry(b'..' + b' ' * 9, 0x10, 0, 0)
    for fname, data in (('1.BMP', data1), ('2.BMP', data2)):
        fc = alloc_chain(len(data))
        write_clusters(fc, data)
        dir_blob += entry(name83(fname), 0x20, fc[0], len(data))
    write_clusters(disp_clusters, bytes(dir_blob))

    # ---- root directory: DISP ----
    img[ROOT_OFF:ROOT_OFF + 32] = entry(name83('DISP'), 0x10, disp_cluster, 0)

    open(out, 'wb').write(img)
    print("wrote %s: %d bytes, spf=%d data_start_sec=%d clusters_used=%d"
          % (out, len(img), spf, data_start, next_free[0] - 2))

if __name__ == "__main__":
    main()
