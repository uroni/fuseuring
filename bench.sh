#!/bin/bash

set -ex

FMNT=/media/test
BMNT=/media/bench
mkdir -p "$FMNT"
mkdir -p "$BMNT"
./fuseuring /tmp/backing_file.img "$FMNT" $((500*1024*1024)) 1000 5000 1 &
while ! test -e "$FMNT/volume"; do sleep 1; done
LODEV=$(losetup --find --show "$FMNT/volume" --direct-io=on)
mkfs.ext4 -F $LODEV
mount $LODEV "$BMNT"
losetup -d $LODEV

cp /usr/share/doc/fio/examples/ssd-test.fio ./
sed -i 's/iodepth=4/iodepth=2048/g' ssd-test.fio
sed -i 's/size=10g/size=400m/g' ssd-test.fio
sed -i 's/libaio/io_uring/g' ssd-test.fio
sed -i "s@directory=/mount-point-of-ssd@directory=$BMNT@g" ssd-test.fio
fio ssd-test.fio

umount "$BMNT"
umount "$FMNT"

