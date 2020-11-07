## Fuseuring

This is an example program that demonstrates how to use io_uring to drive Linux fuse. The only thing it does is forward a single file to the fuse mount point.

### Why?

Using io_uring reduces the number of system calls the fuse program makes which should speed it up.

Fuse is sometimes used to export a single file to be used as volume, as well. E.g. vdfuse, [s3backer](https://github.com/archiecobbs/s3backer), [UrBackup](https://www.urbackup.org/) (vhd/vhdz mounting). Recent improvements in loop (direct-io), fuse and Linux memory management (`PR_SET_IO_FLUSHER`) have made this really performant.

### Performance

With large iodepth it gets good results (backing file on tmpfs):

```
fio ~/fio/ssd-test.fio
seq-read: (g=0): rw=read, bs=(R) 4096B-4096B, (W) 4096B-4096B, (T) 4096B-4096B, ioengine=libaio, iodepth=1024
rand-read: (g=1): rw=randread, bs=(R) 4096B-4096B, (W) 4096B-4096B, (T) 4096B-4096B, ioengine=libaio, iodepth=1024
seq-write: (g=2): rw=write, bs=(R) 4096B-4096B, (W) 4096B-4096B, (T) 4096B-4096B, ioengine=libaio, iodepth=1024
rand-write: (g=3): rw=randwrite, bs=(R) 4096B-4096B, (W) 4096B-4096B, (T) 4096B-4096B, ioengine=libaio, iodepth=1024
fio-3.21
Starting 4 processes
Jobs: 1 (f=1): [_(3),w(1)][100.0%][w=384MiB/s][w=98.3k IOPS][eta 00m:00s]
seq-read: (groupid=0, jobs=1): err= 0: pid=178089: Sat Nov  7 16:39:44 2020
  read: IOPS=165k, BW=643MiB/s (674MB/s)(3500MiB/5443msec)
    slat (nsec): min=1000, max=9680.6k, avg=4407.18, stdev=18980.57
    clat (usec): min=318, max=33373, avg=6190.66, stdev=3064.92
     lat (usec): min=320, max=33379, avg=6195.14, stdev=3066.45
    clat percentiles (usec):
     |  1.00th=[ 1778],  5.00th=[ 2573], 10.00th=[ 2999], 20.00th=[ 3621],
     | 30.00th=[ 4228], 40.00th=[ 4817], 50.00th=[ 5473], 60.00th=[ 6259],
     | 70.00th=[ 7308], 80.00th=[ 8586], 90.00th=[10421], 95.00th=[11994],
     | 99.00th=[14222], 99.50th=[15401], 99.90th=[26608], 99.95th=[28967],
     | 99.99th=[32637]
   bw (  KiB/s): min=614544, max=745196, per=100.00%, avg=662270.43, stdev=49887.45, samples=7
   iops        : min=153636, max=186299, avg=165567.43, stdev=12471.72, samples=7
  lat (usec)   : 500=0.01%, 750=0.02%, 1000=0.06%
  lat (msec)   : 2=1.54%, 4=24.67%, 10=61.52%, 20=11.89%, 50=0.30%
  cpu          : usr=18.83%, sys=43.37%, ctx=84144, majf=0, minf=1036
  IO depths    : 1=0.1%, 2=0.1%, 4=0.1%, 8=0.1%, 16=0.1%, 32=0.1%, >=64=99.9%
     submit    : 0=0.0%, 4=100.0%, 8=0.0%, 16=0.0%, 32=0.0%, 64=0.0%, >=64=0.0%
     complete  : 0=0.0%, 4=100.0%, 8=0.0%, 16=0.0%, 32=0.0%, 64=0.0%, >=64=0.1%
     issued rwts: total=896000,0,0,0 short=0,0,0,0 dropped=0,0,0,0
     latency   : target=0, window=0, percentile=100.00%, depth=1024
rand-read: (groupid=1, jobs=1): err= 0: pid=178096: Sat Nov  7 16:39:44 2020
  read: IOPS=91.0k, BW=356MiB/s (373MB/s)(3500MiB/9845msec)
    slat (nsec): min=1100, max=16238k, avg=9354.69, stdev=93603.08
    clat (usec): min=189, max=47250, avg=11192.69, stdev=2888.77
     lat (usec): min=192, max=47252, avg=11202.13, stdev=2890.28
    clat percentiles (usec):
     |  1.00th=[ 6521],  5.00th=[ 7635], 10.00th=[ 8225], 20.00th=[ 8979],
     | 30.00th=[ 9634], 40.00th=[10290], 50.00th=[10814], 60.00th=[11469],
     | 70.00th=[12125], 80.00th=[13042], 90.00th=[14353], 95.00th=[15664],
     | 99.00th=[20317], 99.50th=[22938], 99.90th=[34866], 99.95th=[42730],
     | 99.99th=[44303]
   bw (  KiB/s): min=333610, max=383504, per=100.00%, avg=365092.00, stdev=16877.24, samples=13
   iops        : min=83402, max=95876, avg=91272.69, stdev=4219.28, samples=13
  lat (usec)   : 250=0.01%, 500=0.01%, 750=0.01%, 1000=0.01%
  lat (msec)   : 2=0.06%, 4=0.16%, 10=34.80%, 20=63.89%, 50=1.06%
  cpu          : usr=11.62%, sys=27.98%, ctx=128214, majf=0, minf=1037
  IO depths    : 1=0.1%, 2=0.1%, 4=0.1%, 8=0.1%, 16=0.1%, 32=0.1%, >=64=99.9%
     submit    : 0=0.0%, 4=100.0%, 8=0.0%, 16=0.0%, 32=0.0%, 64=0.0%, >=64=0.0%
     complete  : 0=0.0%, 4=100.0%, 8=0.0%, 16=0.0%, 32=0.0%, 64=0.0%, >=64=0.1%
     issued rwts: total=896000,0,0,0 short=0,0,0,0 dropped=0,0,0,0
     latency   : target=0, window=0, percentile=100.00%, depth=1024
seq-write: (groupid=2, jobs=1): err= 0: pid=178107: Sat Nov  7 16:39:44 2020
  write: IOPS=131k, BW=513MiB/s (538MB/s)(3500MiB/6818msec); 0 zone resets
    slat (nsec): min=1100, max=4008.6k, avg=5821.89, stdev=18205.65
    clat (usec): min=239, max=21808, avg=7749.25, stdev=3409.43
     lat (usec): min=243, max=21809, avg=7755.16, stdev=3411.55
    clat percentiles (usec):
     |  1.00th=[ 2638],  5.00th=[ 3163], 10.00th=[ 3654], 20.00th=[ 4555],
     | 30.00th=[ 5407], 40.00th=[ 6390], 50.00th=[ 7373], 60.00th=[ 8356],
     | 70.00th=[ 9372], 80.00th=[10683], 90.00th=[12256], 95.00th=[14091],
     | 99.00th=[17171], 99.50th=[17957], 99.90th=[19792], 99.95th=[20317],
     | 99.99th=[21103]
   bw (  KiB/s): min=479727, max=612688, per=99.28%, avg=521884.56, stdev=40381.06, samples=9
   iops        : min=119931, max=153172, avg=130470.78, stdev=10095.48, samples=9
  lat (usec)   : 250=0.01%, 500=0.01%, 750=0.01%, 1000=0.01%
  lat (msec)   : 2=0.16%, 4=13.54%, 10=61.47%, 20=24.72%, 50=0.08%
  cpu          : usr=17.10%, sys=39.93%, ctx=112618, majf=0, minf=14
  IO depths    : 1=0.1%, 2=0.1%, 4=0.1%, 8=0.1%, 16=0.1%, 32=0.1%, >=64=99.9%
     submit    : 0=0.0%, 4=100.0%, 8=0.0%, 16=0.0%, 32=0.0%, 64=0.0%, >=64=0.0%
     complete  : 0=0.0%, 4=100.0%, 8=0.0%, 16=0.0%, 32=0.0%, 64=0.0%, >=64=0.1%
     issued rwts: total=0,896000,0,0 short=0,0,0,0 dropped=0,0,0,0
     latency   : target=0, window=0, percentile=100.00%, depth=1024
rand-write: (groupid=3, jobs=1): err= 0: pid=178115: Sat Nov  7 16:39:44 2020
  write: IOPS=103k, BW=403MiB/s (422MB/s)(3500MiB/8695msec); 0 zone resets
    slat (nsec): min=1200, max=16351k, avg=8084.77, stdev=67434.88
    clat (usec): min=1126, max=38086, avg=9877.97, stdev=2952.08
     lat (usec): min=1128, max=38088, avg=9886.14, stdev=2953.72
    clat percentiles (usec):
     |  1.00th=[ 6325],  5.00th=[ 7373], 10.00th=[ 7701], 20.00th=[ 8094],
     | 30.00th=[ 8455], 40.00th=[ 8848], 50.00th=[ 9110], 60.00th=[ 9503],
     | 70.00th=[10159], 80.00th=[11076], 90.00th=[12518], 95.00th=[14615],
     | 99.00th=[23725], 99.50th=[26870], 99.90th=[32637], 99.95th=[35390],
     | 99.99th=[36963]
   bw (  KiB/s): min=340472, max=447488, per=99.79%, avg=411309.00, stdev=31900.28, samples=17
   iops        : min=85118, max=111872, avg=102827.24, stdev=7975.05, samples=17
  lat (msec)   : 2=0.04%, 4=0.28%, 10=67.14%, 20=30.73%, 50=1.82%
  cpu          : usr=13.52%, sys=30.96%, ctx=74227, majf=0, minf=12
  IO depths    : 1=0.1%, 2=0.1%, 4=0.1%, 8=0.1%, 16=0.1%, 32=0.1%, >=64=99.9%
     submit    : 0=0.0%, 4=100.0%, 8=0.0%, 16=0.0%, 32=0.0%, 64=0.0%, >=64=0.0%
     complete  : 0=0.0%, 4=100.0%, 8=0.0%, 16=0.0%, 32=0.0%, 64=0.0%, >=64=0.1%
     issued rwts: total=0,896000,0,0 short=0,0,0,0 dropped=0,0,0,0
     latency   : target=0, window=0, percentile=100.00%, depth=1024

Run status group 0 (all jobs):
   READ: bw=643MiB/s (674MB/s), 643MiB/s-643MiB/s (674MB/s-674MB/s), io=3500MiB (3670MB), run=5443-5443msec

Run status group 1 (all jobs):
   READ: bw=356MiB/s (373MB/s), 356MiB/s-356MiB/s (373MB/s-373MB/s), io=3500MiB (3670MB), run=9845-9845msec

Run status group 2 (all jobs):
  WRITE: bw=513MiB/s (538MB/s), 513MiB/s-513MiB/s (538MB/s-538MB/s), io=3500MiB (3670MB), run=6818-6818msec

Run status group 3 (all jobs):
  WRITE: bw=403MiB/s (422MB/s), 403MiB/s-403MiB/s (422MB/s-422MB/s), io=3500MiB (3670MB), run=8695-8695msec

Disk stats (read/write):
  loop0: ios=1363439/1466332, merge=428561/321891, ticks=1868225/1741830, in_queue=3610054, util=94.74%
```

### How to compile

Need gcc >= 10 for C++ coroutines. Depends on (recent) liburing-dev.

```bash
autoreconf --install
./configure
make
```

### How to run

Needs a recent Linux kernel (>=5.9) for io_uring functionality and recent `losetup`.

```bash
FMNT=/media/test
BMNT=/media/bench
mkdir -p "$FMNT"
mkdir -p "$BMNT"
./fuseuring /tmp/backing_file.img "$FMNT" $((500*1024*1024)) 5000 &
LODEV=$(losetup --find --show "$FMNT/volume" --direct-io=on)
mkfs.ext4 $LODEV || true
mount $LODEV "$BMNT"
losetup -d $LODEV
```

Or see `bench.sh`.
