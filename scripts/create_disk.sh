#!/bin/bash
set -e

echo ">> Creating 32MB disk.img..."
dd if=/dev/zero of=disk.img bs=1M count=32

echo ">> Formatting disk.img as FAT32..."
mkfs.vfat -F 32 disk.img

echo ">> Creating teste.txt..."
echo "Ola do disco rigido formatado em FAT32!" > teste.txt

echo ">> Copying teste.txt to disk.img..."
mcopy -i disk.img teste.txt ::/teste.txt

echo ">> Cleaning up local teste.txt..."
rm teste.txt

echo ">> Success! disk.img created and populated."
