#!/bin/bash
# Encryption: ZFS dataset with encryption=on (AES-256-GCM) vs a plain pool.
# Two 256 MB file-backed pools; the encrypted dataset is created inside the
# first one with a passphrase key. Run from the vdbench directory; results
# go to encryptionTestOutputNNN/.
set -euo pipefail

SIZE=256M
ON=zfs_encrypt
OFF=zfs_noencrypt
PASSPHRASE=12345678          # test key only (minimum length is 8)

truncate -s "$SIZE" ~/$ON.img
truncate -s "$SIZE" ~/$OFF.img

sudo zpool create -m /$ON  $ON  ~/$ON.img
sudo zpool create -m /$OFF $OFF ~/$OFF.img
echo "$PASSPHRASE" | sudo zfs create -o encryption=on -o keyformat=passphrase $ON/encrypted

sudo ./vdbench -f encryption_test.conf -o encryptionTestOutput+

sudo zfs get -H -o name,property,value encryption,keyformat,used $ON/encrypted $OFF
# Is AES hardware acceleration available? (it makes AES-GCM much cheaper)
grep -q -w aes /proc/cpuinfo && echo "CPU has AES-NI" || echo "CPU has no AES-NI"
