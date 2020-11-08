ACLOCAL_AMFLAGS = -I m4
bin_PROGRAMS = fuseuring

fuseuring_SOURCES = fuse_io_context.cpp main.cpp fuseuring_main.cpp

fuseuring_LDADD = $(PTHREAD_LIBS) -luring
fuseuring_CXXFLAGS = $(PTHREAD_CFLAGS) -std=c++2a -D_FILE_OFFSET_BITS=64
if WITH_FCOROUTINES
fuseuring_CXXFLAGS += -fcoroutines
endif

noinst_HEADERS = fuse_io_context.h fuseuring_main.h fuse_kernel.h
