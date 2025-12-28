#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
FAT32 image generator (empty filesystem).

Creates:
- Optional MBR with one FAT32 partition
- FAT32 Volume Boot Record (boot sector)
- FSInfo sector
- Backup boot sector
- FAT tables (1 or 2)
- Empty root directory cluster

Usage examples:
  python fat32_mkimg.py out.img --size 64MiB --label MYDISK --mbr
  python fat32_mkimg.py out.img --size 256MiB --cluster 8 --sectors-per-fat 2048
"""

from __future__ import annotations
import argparse
import math
import os
import struct
import sys
from dataclasses import dataclass
from typing import Tuple, Optional

# -------------------------
# Helpers
# -------------------------

SIZE_SUFFIXES = {
    "B": 1,
    "K": 1024,
    "KB": 1000,
    "KIB": 1024,
    "M": 1024**2,
    "MB": 1000**2,
    "MIB": 1024**2,
    "G": 1024**3,
    "GB": 1000**3,
    "GIB": 1024**3,
    "T": 1024**4,
    "TB": 1000**4,
    "TIB": 1024**4,
}

def parse_size(s: str) -> int:
    s = s.strip()
    if s.isdigit():
        return int(s)
    # allow "64MiB", "128MB", "1G", "512K", "4096"
    num = ""
    suf = ""
    for ch in s:
        if ch.isdigit() or ch == ".":
            num += ch
        else:
            suf += ch
    if not num:
        raise ValueError(f"Bad size: {s}")
    suf = suf.strip().upper()
    if not suf:
        return int(float(num))
    # normalize common forms
    if suf.endswith("IB") or suf in ("B", "K", "M", "G", "T", "KB", "MB", "GB", "TB", "KIB", "MIB", "GIB", "TIB"):
        mul = SIZE_SUFFIXES.get(suf)
        if mul is None:
            raise ValueError(f"Unknown suffix: {suf}")
        return int(float(num) * mul)
    # accept "MiB" -> already upper => "MIB"
    raise ValueError(f"Unknown size format: {s}")

def le16(x: int) -> bytes:
    return struct.pack("<H", x)

def le32(x: int) -> bytes:
    return struct.pack("<I", x)

def clamp_label(label: str) -> bytes:
    # FAT label is 11 bytes, uppercase, space-padded
    lab = (label.upper()[:11]).ljust(11, " ")
    return lab.encode("ascii", errors="replace")

def div_ceil(a: int, b: int) -> int:
    return (a + b - 1) // b

# -------------------------
# FAT32 layout computation
# -------------------------

@dataclass
class Fat32Params:
    bytes_per_sector: int
    sectors_per_cluster: int
    reserved_sectors: int
    num_fats: int
    total_sectors: int
    sectors_per_fat: int
    root_cluster: int
    hidden_sectors: int
    fat_start_lba: int
    data_start_lba: int
    backup_boot_sector: int
    fsinfo_sector: int
    volume_id: int
    oem_name: bytes
    volume_label: bytes

def compute_sectors_per_fat(
    total_sectors: int,
    bytes_per_sector: int,
    sectors_per_cluster: int,
    reserved_sectors: int,
    num_fats: int
) -> int:
    """
    Iteratively compute FAT size so that:
    data_sectors = total - reserved - num_fats*spf
    cluster_count = data_sectors // spc
    and FAT has enough entries: (cluster_count + 2) * 4 bytes
    spf = ceil( (cluster_count + 2) * 4 / bytes_per_sector )
    """
    # initial rough guess: assume FAT small
    spf = 1
    for _ in range(100):
        data_sectors = total_sectors - reserved_sectors - num_fats * spf
        if data_sectors <= 0:
            raise ValueError("Not enough space for data region. Increase image size or reduce overhead.")
        cluster_count = data_sectors // sectors_per_cluster
        # FAT32 typically requires cluster_count >= 65525; but we don't hard-fail (tiny images may be used for tests)
        needed_fat_bytes = (cluster_count + 2) * 4
        new_spf = div_ceil(needed_fat_bytes, bytes_per_sector)
        if new_spf == spf:
            return spf
        spf = new_spf
    raise RuntimeError("Failed to converge sectors_per_fat")

def cluster_to_lba(params: Fat32Params, cluster: int) -> int:
    # cluster 2 is first data cluster
    if cluster < 2:
        raise ValueError("Cluster numbers < 2 are reserved in FAT32")
    return params.data_start_lba + (cluster - 2) * params.sectors_per_cluster

# -------------------------
# Structures writers
# -------------------------

def make_boot_sector(params: Fat32Params) -> bytes:
    """
    FAT32 BPB + EBPB (512 bytes).
    """
    bps = params.bytes_per_sector
    if bps not in (512, 1024, 2048, 4096):
        raise ValueError("bytes_per_sector must be one of 512/1024/2048/4096")
    if bps != 512:
        # Boot sector still one sector; most images assume 512. Keep code general but warn-ish.
        pass

    jump = b"\xEB\x58\x90"  # JMP + NOP (common for FAT32)
    oem = params.oem_name.ljust(8, b" ")[:8]

    # BPB (offsets as per FAT spec)
    bpb = bytearray()
    bpb += jump
    bpb += oem
    bpb += le16(params.bytes_per_sector)          # BPB_BytsPerSec
    bpb += struct.pack("<B", params.sectors_per_cluster)  # BPB_SecPerClus
    bpb += le16(params.reserved_sectors)          # BPB_RsvdSecCnt
    bpb += struct.pack("<B", params.num_fats)     # BPB_NumFATs
    bpb += le16(0)                                # BPB_RootEntCnt (0 for FAT32)
    bpb += le16(0)                                # BPB_TotSec16 (0 if TotSec32 used)
    bpb += struct.pack("<B", 0xF8)                # BPB_Media (fixed disk)
    bpb += le16(0)                                # BPB_FATSz16 (0 for FAT32)
    bpb += le16(63)                               # BPB_SecPerTrk (fake geometry)
    bpb += le16(255)                              # BPB_NumHeads (fake geometry)
    bpb += le32(params.hidden_sectors)            # BPB_HiddSec
    bpb += le32(params.total_sectors)             # BPB_TotSec32

    # FAT32 extended BPB
    bpb += le32(params.sectors_per_fat)           # BPB_FATSz32
    bpb += le16(0)                                # BPB_ExtFlags
    bpb += le16(0)                                # BPB_FSVer (0.0)
    bpb += le32(params.root_cluster)              # BPB_RootClus
    bpb += le16(params.fsinfo_sector)             # BPB_FSInfo
    bpb += le16(params.backup_boot_sector)        # BPB_BkBootSec
    bpb += b"\x00" * 12                           # BPB_Reserved

    # EBPB
    bpb += struct.pack("<B", 0x80)                # BS_DrvNum
    bpb += struct.pack("<B", 0x00)                # BS_Reserved1
    bpb += struct.pack("<B", 0x29)                # BS_BootSig
    bpb += le32(params.volume_id)                 # BS_VolID
    bpb += params.volume_label                    # BS_VolLab (11)
    bpb += b"FAT32   "                            # BS_FilSysType (8)

    # Bootstrap code area + signature
    if len(bpb) > params.bytes_per_sector - 2:
        raise ValueError("Boot sector fields overflow sector size")
    sector = bytearray(params.bytes_per_sector)
    sector[:len(bpb)] = bpb
    sector[-2:] = b"\x55\xAA"
    return bytes(sector)

def make_fsinfo_sector(params: Fat32Params) -> bytes:
    """
    FAT32 FSInfo sector (usually sector 1 of reserved region).
    Contains signatures + free cluster count (unknown) + next free cluster hint.
    """
    sector = bytearray(params.bytes_per_sector)
    # Signatures per Microsoft FAT spec
    sector[0:4] = b"RRaA"              # LeadSig 0x41615252 (little-endian in spec; but stored as bytes)
    sector[484:488] = b"rrAa"          # StrucSig 0x61417272
    sector[488:492] = le32(0xFFFFFFFF) # Free_Count unknown
    sector[492:496] = le32(0x00000003) # Nxt_Free hint (3 is a common start; root is 2)
    sector[508:512] = b"\x55\xAA"
    return bytes(sector)

def make_mbr(
    total_sectors: int,
    part_start_lba: int,
    part_sectors: int,
    fat32_lba_type: int = 0x0C
) -> bytes:
    """
    Simple MBR with one primary partition entry.
    CHS fields are filled with common 'max' placeholders.
    """
    mbr = bytearray(512)
    # Partition entry starts at 446
    # status, chs_first(3), type, chs_last(3), lba_start(4), sectors(4)
    status = 0x00
    chs_first = b"\xFE\xFF\xFF"
    chs_last = b"\xFE\xFF\xFF"
    ptype = fat32_lba_type
    entry = struct.pack(
        "<B3sB3sII",
        status,
        chs_first,
        ptype,
        chs_last,
        part_start_lba,
        part_sectors
    )
    mbr[446:446+16] = entry
    mbr[510:512] = b"\x55\xAA"
    return bytes(mbr)

def write_at(f, offset: int, data: bytes) -> None:
    f.seek(offset)
    f.write(data)

# -------------------------
# Main build
# -------------------------

def build_params(args) -> Fat32Params:
    bytes_per_sector = args.sector_size
    sectors_per_cluster = args.cluster
    num_fats = args.fats
    reserved_sectors = args.reserved
    root_cluster = args.root_cluster
    fsinfo_sector = args.fsinfo_sector
    backup_boot_sector = args.backup_boot_sector

    if sectors_per_cluster not in (1, 2, 4, 8, 16, 32, 64, 128):
        raise ValueError("sectors_per_cluster must be power-of-two in {1,2,4,8,16,32,64,128}")

    size_bytes = parse_size(args.size)
    if size_bytes % bytes_per_sector != 0:
        raise ValueError("Image size must be multiple of sector size")
    total_sectors = size_bytes // bytes_per_sector

    # If using MBR, we start partition at args.part_start (LBA). Otherwise volume starts at LBA 0.
    hidden_sectors = args.part_start if args.mbr else 0

    # total sectors visible to the volume (partition size)
    volume_total_sectors = total_sectors - hidden_sectors if args.mbr else total_sectors
    if volume_total_sectors <= 0:
        raise ValueError("Partition start beyond image size")

    if args.sectors_per_fat is None:
        sectors_per_fat = compute_sectors_per_fat(
            total_sectors=volume_total_sectors,
            bytes_per_sector=bytes_per_sector,
            sectors_per_cluster=sectors_per_cluster,
            reserved_sectors=reserved_sectors,
            num_fats=num_fats
        )
    else:
        sectors_per_fat = args.sectors_per_fat

    fat_start_lba = hidden_sectors + reserved_sectors
    data_start_lba = fat_start_lba + num_fats * sectors_per_fat

    # Minimal sanity
    if data_start_lba >= (hidden_sectors + volume_total_sectors):
        raise ValueError("Layout invalid: data region starts beyond volume end")

    # Volume ID: simple deterministic-ish (can be overridden)
    volume_id = args.volume_id if args.volume_id is not None else (int.from_bytes(os.urandom(4), "little"))
    oem_name = (args.oem or "MSWIN4.1").encode("ascii", errors="replace")[:8]
    volume_label = clamp_label(args.label)

    return Fat32Params(
        bytes_per_sector=bytes_per_sector,
        sectors_per_cluster=sectors_per_cluster,
        reserved_sectors=reserved_sectors,
        num_fats=num_fats,
        total_sectors=volume_total_sectors,
        sectors_per_fat=sectors_per_fat,
        root_cluster=root_cluster,
        hidden_sectors=hidden_sectors,
        fat_start_lba=fat_start_lba,
        data_start_lba=data_start_lba,
        backup_boot_sector=backup_boot_sector,
        fsinfo_sector=fsinfo_sector,
        volume_id=volume_id,
        oem_name=oem_name,
        volume_label=volume_label,
    )

def init_fat_tables(params: Fat32Params) -> bytes:
    """
    Create the initial FAT contents for one FAT table:
    FAT[0] = media + reserved bits
    FAT[1] = EOC
    FAT[root_cluster] = EOC  (allocate root dir cluster)
    Remaining entries are 0 (free).
    """
    fat_bytes = params.sectors_per_fat * params.bytes_per_sector
    fat = bytearray(fat_bytes)

    # Entry 0: 0x0FFFFFF8 for fixed media (0xF8) + reserved high bits.
    struct.pack_into("<I", fat, 0, 0x0FFFFFF8)
    # Entry 1: EOC
    struct.pack_into("<I", fat, 4, 0x0FFFFFFF)

    # Root dir cluster marked as end-of-chain
    rc = params.root_cluster
    if rc < 2:
        raise ValueError("root_cluster must be >= 2")
    struct.pack_into("<I", fat, rc * 4, 0x0FFFFFFF)

    return bytes(fat)

def zero_cluster(params: Fat32Params) -> bytes:
    return b"\x00" * (params.sectors_per_cluster * params.bytes_per_sector)

def make_root_dir_cluster(params: Fat32Params) -> bytes:
    """
    Create root directory cluster with '.' and '..' entries.
    FAT32 root is a normal directory cluster.
    """
    cluster_size = params.sectors_per_cluster * params.bytes_per_sector
    buf = bytearray(cluster_size)

    def dir_entry(name: bytes, attr: int, first_cluster: int) -> bytes:
        entry = bytearray(32)
        entry[0:11] = name.ljust(11, b' ')
        entry[11] = attr
        # first cluster split (high / low)
        entry[20:22] = le16((first_cluster >> 16) & 0xFFFF)
        entry[26:28] = le16(first_cluster & 0xFFFF)
        return bytes(entry)

    # "." entry
    buf[0:32] = dir_entry(
        name=b".",
        attr=0x10,  # ATTR_DIRECTORY
        first_cluster=params.root_cluster
    )

    # ".." entry (root points to itself)
    buf[32:64] = dir_entry(
        name=b"..",
        attr=0x10,
        first_cluster=params.root_cluster
    )
    
    # ".." entry (root points to itself)
    buf[64:96] = dir_entry(
        name=b"ROOT",
        attr=0x10,
        first_cluster=params.root_cluster
    )

    return bytes(buf)

def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Generate empty FAT32 image.")
    ap.add_argument("--output", help="Path to output image file")
    ap.add_argument("--size", required=True, help="Image size (e.g. 64MiB, 128MB, 1GiB, or bytes)")
    ap.add_argument("--sector-size", type=int, default=512, help="Bytes per sector (default: 512)")
    ap.add_argument("--cluster", type=int, default=8, help="Sectors per cluster (default: 8 => 4KiB @512B sectors)")
    ap.add_argument("--reserved", type=int, default=32, help="Reserved sectors before FAT (default: 32)")
    ap.add_argument("--fats", type=int, default=2, help="Number of FATs (default: 2)")
    ap.add_argument("--sectors-per-fat", type=int, default=None, help="Override FAT size (sectors). If omitted, auto-compute.")
    ap.add_argument("--root-cluster", type=int, default=2, help="Root directory start cluster (default: 2)")
    ap.add_argument("--fsinfo-sector", type=int, default=1, help="FSInfo sector number within reserved region (default: 1)")
    ap.add_argument("--backup-boot-sector", type=int, default=6, help="Backup boot sector within reserved region (default: 6)")
    ap.add_argument("--label", default="NO NAME", help="Volume label (<= 11 chars, default: 'NO NAME')")
    ap.add_argument("--oem", default="MSWIN4.1", help="OEM name (<= 8 chars)")
    ap.add_argument("--volume-id", type=lambda x: int(x, 0), default=None, help="Volume ID (hex like 0x1234ABCD or int)")
    ap.add_argument("--mbr", action="store_true", help="Write MBR with one FAT32 LBA partition")
    ap.add_argument("--part-start", type=int, default=2048, help="Partition start LBA if --mbr (default: 2048)")

    ap.add_argument(
        "--init-root-dir",
        action="store_true",
        help="Initialize root directory with . and .. entries"
    )

    args = ap.parse_args(argv)

    params = build_params(args)

    # Create/truncate file
    size_bytes = parse_size(args.size)
    with open(args.output, "wb") as f:
        f.truncate(size_bytes)

        # Optional MBR
        if args.mbr:
            part_start = args.part_start
            part_sectors = (size_bytes // params.bytes_per_sector) - part_start
            if part_sectors <= 0:
                raise ValueError("Image too small for selected partition start")
            mbr = make_mbr(
                total_sectors=size_bytes // params.bytes_per_sector,
                part_start_lba=part_start,
                part_sectors=part_sectors,
                fat32_lba_type=0x0C
            )
            write_at(f, 0, mbr)

        # Boot sector (VBR) at volume start (hidden_sectors)
        vbr_lba = params.hidden_sectors
        vbr = make_boot_sector(params)
        write_at(f, vbr_lba * params.bytes_per_sector, vbr)

        # FSInfo sector
        fsinfo_lba = params.hidden_sectors + params.fsinfo_sector
        fsinfo = make_fsinfo_sector(params)
        write_at(f, fsinfo_lba * params.bytes_per_sector, fsinfo)

        # Backup boot sector
        bkp_lba = params.hidden_sectors + params.backup_boot_sector
        write_at(f, bkp_lba * params.bytes_per_sector, vbr)

        # Initialize FATs
        one_fat = init_fat_tables(params)
        for i in range(params.num_fats):
            fat_lba = params.fat_start_lba + i * params.sectors_per_fat
            write_at(f, fat_lba * params.bytes_per_sector, one_fat)

        # Zero out root directory cluster
        root_lba = cluster_to_lba(params, params.root_cluster)

        if args.init_root_dir:
            root_data = make_root_dir_cluster(params)
        else:
            root_data = zero_cluster(params)

        write_at(f, root_lba * params.bytes_per_sector, root_data)


    # Small summary to stderr/stdout
    sys.stdout.write(
        "Created FAT32 image:\n"
        f"  file: {args.output}\n"
        f"  size: {size_bytes} bytes\n"
        f"  sector: {params.bytes_per_sector} bytes\n"
        f"  cluster: {params.sectors_per_cluster} sectors\n"
        f"  reserved: {params.reserved_sectors} sectors\n"
        f"  FATs: {params.num_fats}\n"
        f"  sectors/FAT: {params.sectors_per_fat}\n"
        f"  hidden (VBR start LBA): {params.hidden_sectors}\n"
        f"  FAT start LBA: {params.fat_start_lba}\n"
        f"  data start LBA: {params.data_start_lba}\n"
        f"  root cluster: {params.root_cluster} (LBA {cluster_to_lba(params, params.root_cluster)})\n"
        f"  label: {params.volume_label.decode('ascii', errors='replace')}\n"
    )
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
