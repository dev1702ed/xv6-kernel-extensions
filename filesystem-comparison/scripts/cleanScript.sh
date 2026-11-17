#!/bin/bash
# Destroy the test pools and delete their backing files.
# Pools that don't exist are skipped.
for pool in zfs_compress zfs_nocompress zfs_encrypt zfs_noencrypt; do
  if sudo zpool list "$pool" >/dev/null 2>&1; then
    sudo zpool destroy "$pool"
  fi
done
rm -f ~/zfs_compress.img ~/zfs_nocompress.img ~/zfs_encrypt.img ~/zfs_noencrypt.img
