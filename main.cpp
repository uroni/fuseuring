// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) Martin Raiber 
#include "fuseuring_main.h"
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <iostream>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <stdlib.h>

#ifndef PR_SET_IO_FLUSHER
#define PR_SET_IO_FLUSHER 57
#endif

int main(int argc, char* argv[])
{
    if(argc<5)
    {
        std::cerr << "Not enough arguments ./fuseuring [backing file path] [fuse mount path] [backing file size] [fuse max_background]" << std::endl;
        return 101;
    }

    int backing_fd = open(argv[1], O_CLOEXEC|O_CREAT|O_RDWR, S_IRWXU);
    //int backing_fd = memfd_create("backing_file", MFD_CLOEXEC);

    if(backing_fd==-1)
    {
        perror("Error opening backing file");
        return 1;
    }    

    int64_t backing_file_size = atoll(argv[3]);

    int rc = posix_fallocate(backing_fd, 0, backing_file_size);
    if(rc!=0)
    {
        std::cerr << "Error allocating 1GB for backing file rc: " << rc << std::endl;
        return 1;
    }

    rc = prctl(PR_SET_IO_FLUSHER, 1, 0, 0, 0);

    if(rc!=0)
    {
        perror("Error setting PR_SET_IO_FLUSHER");
    }

    struct rlimit rlimit;
    rlimit.rlim_cur = RLIM_INFINITY;
    rlimit.rlim_max = RLIM_INFINITY;
    rc = setrlimit(RLIMIT_MEMLOCK, &rlimit);
    if(rc!=0)
    {
        perror("Error increasing RLIMIT_MEMLOCK");
    }

    int fuse_max_background = atoi(argv[4]);

    return fuseuring_main(backing_fd, argv[2], fuse_max_background, fuse_max_background+1000);
}
