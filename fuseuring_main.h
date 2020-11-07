// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) Martin Raiber
#pragma once
#include <string>

int fuseuring_main(int backing_fd, const std::string& mountpoint, int max_background, int congestion_threshold);