ACLOCAL_AMFLAGS = -I m4
bin_PROGRAMS = fuseuring

fuseuring_SOURCES = fuse_io_service.cpp main.cpp fuseuring_main.cpp

fuseuring_LDADD = $(PTHREAD_LIBS) -luring
fuseuring_CXXFLAGS = $(PTHREAD_CFLAGS) -std=c++2a -D_FILE_OFFSET_BITS=64

noinst_HEADERS = fuse_io_service.h fuseuring_main.h fuse_kernel.h
