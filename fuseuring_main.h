// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) Martin Raiber
#pragma once
#include <string>

int fuseuring_main(int backing_fd, const std::string& mountpoint, int max_fuse_ios, 
    int max_background, int congestion_threshold, size_t n_threads);

struct fuse_uring;
int fuseuring_run(int max_fuse_ios, size_t max_write, int backing_fd, 
    int fuse_fd, struct io_uring* fuse_uring, int uring_wq_fd);