#!/bin/bash
# Compression: ZFS with compression=lz4 vs compression=off.
# Two 256 MB file-backed pools (so the real disks are never touched),
# then one vdbench run that writes and reads the same workload on both.
# Run from the vdbench directory; results go to compressionTestOutputNNN/.
set -euo pipefail

SIZE=256M
ON=zfs_compress
OFF=zfs_nocompress

truncate -s "$SIZE" ~/$ON.img
truncate -s "$SIZE" ~/$OFF.img

sudo zpool create -m /$ON  $ON  ~/$ON.img
sudo zpool create -m /$OFF $OFF ~/$OFF.img
sudo zfs set compression=lz4 $ON
sudo zfs set compression=off $OFF

sudo ./vdbench -f compression_test.conf -o compressionTestOutput+

# Space actually used: physical (used) vs logical (logicalused) and ratio.
sudo zfs get -H -o name,property,value used,logicalused,compressratio $ON $OFF
df -h /$ON /$OFF
