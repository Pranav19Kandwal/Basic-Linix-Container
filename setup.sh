#!/bin/bash
set -e

# Root filesystem path and BusyBox binary location
ROOTFS="./rootfs"
BUSYBOX="/usr/bin/busybox"

echo "[*] Preparing minimal rootfs at $ROOTFS"

# Re-creating standard root filesystem directories
rm -rf "$ROOTFS"
mkdir -p "$ROOTFS"/{bin,etc,lib,lib64,proc,sys,dev,tmp}

# Ensuring BusyBox exists
if [ ! -x "$BUSYBOX" ]; then
    echo "[!] BusyBox binary not found or not executable at $BUSYBOX"
    exit 1
fi

# Copying busybox and its dependencies
mkdir -p "$ROOTFS/bin"
cp -v "$BUSYBOX" "$ROOTFS/bin/busybox"

# Creating symlinks for common commands to BusyBox in rootfs
for cmd in sh ls ps hostname; do
    ln -sv busybox "$ROOTFS/bin/$cmd"
done

# Copying shared library dependencies of BusyBox into rootfs
ldd "$BUSYBOX" | awk '{ if ($(NF-1) == "=>") print $3; else if ($1 ~ /^\//) print $1; }' | while read lib; do
    [ -e "$lib" ] || continue
    mkdir -p "$ROOTFS$(dirname "$lib")"
    cp -v "$lib" "$ROOTFS$lib"
done

# Creating etc directory inside rootfs
mkdir -p "$ROOTFS/etc"

# Creating /etc/passwd with root user(UID 0, GID 0)
cat > $ROOTFS/etc/passwd <<EOF
root:x:0:0:root:/root:/bin/sh
EOF

# Creating /etc/group with root group(GID 0)
cat > $ROOTFS/etc/group <<EOF
root:x:0:
EOF

echo "[*] Minimal BusyBox-based rootfs ready in $ROOTFS"