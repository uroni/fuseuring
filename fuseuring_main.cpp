// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) Martin Raiber 
#include <sys/mount.h>
#include "fuse_kernel.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <liburing.h>
#include <stack>
#include <malloc.h>
#include <assert.h>
#include <memory.h>
#include <thread>
#include <iostream>
#include "fuse_io_context.h"
#include "fuseuring_main.h"

namespace
{
    const size_t fuse_max_pages = 256;  
    const size_t header_buf_size = std::max(sizeof(fuse_in_header) + sizeof(fuse_write_in), 
                                        sizeof(fuse_out_header) + sizeof(fuse_write_out));
    const size_t scratch_buf_size = std::max(std::max(std::max(
                        static_cast<size_t>(4096), 
                        sizeof(fuse_out_header)+sizeof(fuse_attr_out)),
                        sizeof(fuse_out_header)+sizeof(fuse_entry_out)),
                        sizeof(fuse_out_header)+sizeof(fuse_write_out));

    template<typename T>
    auto round_up(T numToRound, T multiple)
	{
		return ((numToRound + multiple - 1) / multiple) * multiple;
	}
}

[[nodiscard]] fuse_io_context::io_uring_task<char*> read_rbytes(fuse_io_context& io, fuse_io_context::FuseIoVal& fuse_io,
    size_t rbytes, bool add_zero, size_t init_read, std::vector<char>& free_buf)
{
    if(rbytes==0)
    {
        co_return fuse_io->scratch_buf;
    }

    bool io_read=true;
    if(init_read==rbytes)
    {
        if(add_zero)
            io_read=false;
        else
            co_return fuse_io->header_buf + sizeof(fuse_in_header);
    }

    bool read_fixed=false;
    if(rbytes + (add_zero ? 1 : 0) < scratch_buf_size)
    {
        if(init_read>0)
            memcpy(fuse_io->scratch_buf, fuse_io->header_buf + sizeof(fuse_in_header), init_read);

        read_fixed=true;
    }
    else
    {
        free_buf.resize(rbytes + (add_zero ? 1 : 0));
        
        if(init_read>0)
            memcpy(free_buf.data(), fuse_io->header_buf + sizeof(fuse_in_header), init_read);
    }

    if(io_read)
    {
        size_t read_done = init_read;
        do
        {
            io_uring_sqe *sqe = io.get_sqe();
            if(sqe==nullptr)
                co_return nullptr;

            DBG_PRINT(std::cout << "Read rbytes "<< rbytes << " init read " << init_read << std::endl);
            
            if(read_fixed)
            {
                io_uring_prep_read_fixed(sqe, fuse_io->pipe[0], fuse_io->scratch_buf + read_done,
                        rbytes - read_done, 0, fuse_io->scratch_buf_idx);
            }
            else
            {
                io_uring_prep_read(sqe, fuse_io->pipe[0], &free_buf[read_done],
                            rbytes-read_done, 0);
            }

            sqe->flags |= IOSQE_FIXED_FILE;
            int rc = co_await io.complete(sqe);

            if(rc<=0)
            {
                co_return nullptr;
            }

            read_done+=rc;

        } while(read_done<rbytes);
    }

    if(read_fixed)
    {
        if(add_zero)
            fuse_io->scratch_buf[rbytes] = 0;

        co_return fuse_io->scratch_buf;
    }
    else
    {
        if(add_zero)
            free_buf[rbytes]=0;

        co_return &free_buf[0];
    }
}

[[nodiscard]] fuse_io_context::io_uring_task<int> send_reply(fuse_io_context& io, fuse_io_context::FuseIoVal& fuse_io)
{
    struct io_uring_sqe *sqe;
    sqe = io.get_sqe(2);
    if(sqe==nullptr)
        co_return -1;

    size_t reply_size = reinterpret_cast<const fuse_out_header*>(fuse_io->scratch_buf)->len;

    io_uring_prep_write_fixed(sqe, fuse_io->pipe[1],
            fuse_io->scratch_buf, reply_size,
            0, fuse_io->scratch_buf_idx);
    sqe->flags |= IOSQE_FIXED_FILE | IOSQE_IO_LINK;

    io_uring_sqe *sqe2 = io.get_sqe();

    io_uring_prep_splice(sqe2, fuse_io->pipe[0],
        -1, io.fuse_ring.fd, -1, reply_size,
        SPLICE_F_MOVE| SPLICE_F_FD_IN_FIXED | SPLICE_F_NONBLOCK);
    sqe2->flags |= IOSQE_FIXED_FILE;
    
    auto [rc1, rc2] = co_await io.complete(std::make_pair(sqe, sqe2));

    if(rc1!=reply_size || rc2!=reply_size)
    {
        std::cerr << "# Send reply failed rc1="<< rc1 << " rc2=" << rc2 << std::endl;
        co_return -1;
    }
    else
    {
        co_return 0;
    }
}

[[nodiscard]] fuse_io_context::io_uring_task<int> send_reply(fuse_io_context& io, fuse_io_context::FuseIoVal& fuse_io,
    const std::vector<char>& buf)
{
    struct io_uring_sqe *sqe;
    sqe = io.get_sqe(2);
    if(sqe==nullptr)
        co_return -1;

    DBG_PRINT(std::cout << "send unique buf: " << reinterpret_cast<const fuse_out_header*>(buf.data())->unique << std::endl);
    io_uring_prep_write(sqe, fuse_io->pipe[1],
            buf.data(), buf.size(),
            0);
    sqe->flags |= IOSQE_FIXED_FILE | IOSQE_IO_LINK;

    io_uring_sqe *sqe2 = io.get_sqe();
    if(sqe2==nullptr)
        co_return -1;

    io_uring_prep_splice(sqe2, fuse_io->pipe[0],
        -1, io.fuse_ring.fd, -1, buf.size(),
        SPLICE_F_MOVE| SPLICE_F_FD_IN_FIXED | SPLICE_F_NONBLOCK);
    sqe2->flags |= IOSQE_FIXED_FILE;
    
    auto [rc1, rc2] = co_await io.complete(std::make_pair(sqe, sqe2));

    if(rc1!=buf.size() || rc2!=buf.size())
    {
        std::cerr << "# Send reply buf failed rc1="<< rc1 << " rc2=" << rc2 << std::endl;
        co_return -1;
    }
    else
    {
        co_return 0;
    }
}

[[nodiscard]] fuse_io_context::io_uring_task<int> handle_unknown(fuse_io_context& io, fuse_io_context::FuseIoVal& fuse_io)
{
    fuse_in_header* fheader = reinterpret_cast<fuse_in_header*>(fuse_io->header_buf);
    fuse_out_header* out_header = reinterpret_cast<fuse_out_header*>(fuse_io->scratch_buf);
   
    out_header->error = -ENOSYS;
    out_header->len = sizeof(fuse_out_header);
    out_header->unique = fheader->unique;    

    co_return co_await send_reply(io, fuse_io);
}

[[nodiscard]] fuse_io_context::io_uring_task<int> send_attr(fuse_io_context& io, fuse_io_context::FuseIoVal& fuse_io,
    uint64_t unique, uint64_t nodeid)
{
    fuse_out_header* out_header = reinterpret_cast<fuse_out_header*>(fuse_io->scratch_buf);
    out_header->error = 0;
    out_header->len = sizeof(fuse_attr_out) + sizeof(fuse_out_header);
    out_header->unique = unique;

    fuse_attr_out* attr_out = reinterpret_cast<fuse_attr_out*>(fuse_io->scratch_buf + sizeof(fuse_out_header));
    attr_out->attr_valid = 3600;
    attr_out->attr_valid_nsec = 0;
    memset(&attr_out->attr, 0, sizeof(attr_out->attr));

    DBG_PRINT(std::cout << "send_attr nodeid " << nodeid << std::endl);
    if(nodeid==1)
    {
        attr_out->attr.mode = S_IFDIR | S_IRWXU | S_IRWXG | S_IRWXO;
        attr_out->attr.ino = 1;
    }
    else if(nodeid==3)
    {
        attr_out->attr.mode = S_IFREG | S_IRWXU | S_IRWXG | S_IRWXO;
        attr_out->attr.ino = 3;
        attr_out->attr.size = io.fuse_ring.backing_f_size;
        attr_out->attr.blocks = round_up<off_t>(attr_out->attr.size, 512);
        attr_out->attr.blksize = getpagesize();
    }
    else
    {
        out_header->error = -EACCES;
        out_header->len = sizeof(fuse_out_header);
    }

    co_return co_await send_reply(io, fuse_io);
}

[[nodiscard]] fuse_io_context::io_uring_task<int> handle_getattr(fuse_io_context& io, fuse_io_context::FuseIoVal& fuse_io,
    char* rbytes_buf)
{
    fuse_in_header* fheader = reinterpret_cast<fuse_in_header*>(fuse_io->header_buf);
    uint64_t nodeid = fheader->nodeid;

    fuse_getattr_in* getattr_in = reinterpret_cast<fuse_getattr_in*>(rbytes_buf);
    bool getattr_fh = (getattr_in->getattr_flags & FUSE_GETATTR_FH)>0;
    if(getattr_fh)
    {
        DBG_PRINT(std::cout << "fattr fh" << std::endl);
        nodeid = getattr_in->fh;
    }

    co_return co_await send_attr(io, fuse_io, fheader->unique, nodeid);
}

[[nodiscard]] fuse_io_context::io_uring_task<int> handle_setattr(fuse_io_context& io, fuse_io_context::FuseIoVal& fuse_io,
    char* rbytes_buf)
{
    fuse_in_header* fheader = reinterpret_cast<fuse_in_header*>(fuse_io->header_buf);
    uint64_t nodeid = fheader->nodeid;

    fuse_setattr_in* setattr_in = reinterpret_cast<fuse_setattr_in*>(rbytes_buf);
    if(setattr_in->fh)
    {
        DBG_PRINT(std::cout << "fattr fh" << std::endl);
        nodeid = setattr_in->fh;
    }

    if(nodeid==3)
    {
        DBG_PRINT(std::cout << "Set attr new size " << setattr_in->size << " denied" << std::endl);
    }

    co_return co_await send_attr(io, fuse_io, fheader->unique, nodeid);
}

[[nodiscard]] fuse_io_context::io_uring_task<int> handle_lookup(fuse_io_context& io, fuse_io_context::FuseIoVal& fuse_io,
    char* rbytes_buf)
{
    fuse_in_header* fheader = reinterpret_cast<fuse_in_header*>(fuse_io->header_buf);

    DBG_PRINT(std::cout << "fuse lookup " << rbytes_buf << std::endl);

    std::string lname = rbytes_buf;

    fuse_out_header* out_header = reinterpret_cast<fuse_out_header*>(fuse_io->scratch_buf);
    out_header->error = 0;
    out_header->len = sizeof(fuse_entry_out) + sizeof(fuse_out_header);
    out_header->unique = fheader->unique;

    fuse_entry_out* entry_out = reinterpret_cast<fuse_entry_out*>(fuse_io->scratch_buf + sizeof(fuse_out_header));
    
    entry_out->generation = 0;
    entry_out->entry_valid = 3600;
    entry_out->entry_valid_nsec = 0;
    entry_out->attr_valid = 3600;
    entry_out->attr_valid_nsec = 0;

    if(lname=="volume")
    {
        DBG_PRINT(std::cout << "Looking up volume" << std::endl);
        entry_out->nodeid = 3;
        entry_out->attr = {};
        entry_out->attr.mode = S_IFREG | S_IRWXU | S_IRWXG | S_IRWXO;
        entry_out->attr.ino = 3;
        entry_out->attr.size = io.fuse_ring.backing_f_size;
        entry_out->attr.blocks = round_up<off_t>(entry_out->attr.size, 512);
        entry_out->attr.blksize = getpagesize();
    }
    else
    {
        entry_out->nodeid = 1;
        entry_out->attr = {};
        entry_out->attr.mode = S_IFDIR | S_IRWXU | S_IRWXG | S_IRWXO;
        entry_out->attr.ino = 1;   
    }    

    co_return co_await send_reply(io, fuse_io);
}

[[nodiscard]] fuse_io_context::io_uring_task<int> handle_opendir(fuse_io_context& io, fuse_io_context::FuseIoVal& fuse_io,
    char* rbytes_buf)
{
    fuse_in_header* fheader = reinterpret_cast<fuse_in_header*>(fuse_io->header_buf);
    fuse_open_in* open_in = reinterpret_cast<fuse_open_in*>(rbytes_buf);

    DBG_PRINT(std::cout << "opendir nodeid " << fheader->nodeid << std::endl);

    fuse_out_header* out_header = reinterpret_cast<fuse_out_header*>(fuse_io->scratch_buf);
    out_header->error = 0;
    out_header->len = sizeof(fuse_open_out) + sizeof(fuse_out_header);
    out_header->unique = fheader->unique;

    fuse_open_out* open_out = reinterpret_cast<fuse_open_out*>(fuse_io->scratch_buf + sizeof(fuse_out_header));
    open_out->fh = 1;
    open_out->open_flags = open_in->flags | FOPEN_CACHE_DIR;
    
    co_return co_await send_reply(io, fuse_io);
}

[[nodiscard]] fuse_io_context::io_uring_task<int> handle_open(fuse_io_context& io, fuse_io_context::FuseIoVal& fuse_io,
    char* rbytes_buf)
{
    fuse_in_header* fheader = reinterpret_cast<fuse_in_header*>(fuse_io->header_buf);
    fuse_open_in* open_in = reinterpret_cast<fuse_open_in*>(rbytes_buf);

    DBG_PRINT(std::cout << "open nodeid " << fheader->nodeid << std::endl);

    fuse_out_header* out_header = reinterpret_cast<fuse_out_header*>(fuse_io->scratch_buf);
    out_header->error = 0;
    out_header->len = sizeof(fuse_open_out) + sizeof(fuse_out_header);
    out_header->unique = fheader->unique;

    fuse_open_out* open_out = reinterpret_cast<fuse_open_out*>(fuse_io->scratch_buf + sizeof(fuse_out_header));
    open_out->fh = 3;
    open_out->open_flags = open_in->flags | FOPEN_KEEP_CACHE | FOPEN_DIRECT_IO;
    
    co_return co_await send_reply(io, fuse_io);
}

[[nodiscard]] fuse_io_context::io_uring_task<int> handle_read(fuse_io_context& io, fuse_io_context::FuseIoVal& fuse_io,
    char* rbytes_buf)
{
    fuse_in_header* fheader = reinterpret_cast<fuse_in_header*>(fuse_io->header_buf);
    uint64_t read_offset;
    uint32_t read_size;
    {
        fuse_read_in* read_in = reinterpret_cast<fuse_read_in*>(rbytes_buf);

        DBG_PRINT(std::cout << "read nodeid " << fheader->nodeid << " off: " << read_in->offset << " size: "<<read_in->size << std::endl);

        if(fheader->nodeid!=3)
        {
            fuse_out_header* out_header = reinterpret_cast<fuse_out_header*>(fuse_io->scratch_buf);
            out_header->error = 0;
            out_header->unique = fheader->unique;
            out_header->error = -ENOENT;
            co_return co_await send_reply(io, fuse_io);
        }

        if(read_in->offset + read_in->size > io.fuse_ring.backing_f_size)
        {
            read_in->size = io.fuse_ring.backing_f_size - read_in->offset;
            DBG_PRINT(std::cout << "Reading less: " << read_in->size << std::endl);
        }

        read_offset = read_in->offset;
        read_size = read_in->size;
    }

    fuse_out_header* out_header = reinterpret_cast<fuse_out_header*>(fuse_io->scratch_buf);
    out_header->error = 0;
    out_header->len = sizeof(fuse_out_header) + read_size;
    out_header->unique = fheader->unique;

    io_uring_sqe* sqe1 = io.get_sqe(3);
    if(sqe1==nullptr)
        co_return -1;

    io_uring_prep_write_fixed(sqe1, fuse_io->pipe[1],
            fuse_io->scratch_buf, sizeof(fuse_out_header),
            -1, fuse_io->scratch_buf_idx);
    sqe1->flags |= IOSQE_FIXED_FILE | IOSQE_IO_LINK;

    io_uring_sqe *sqe2 = io.get_sqe();
    if(sqe2==nullptr)
        co_return -1;

    io_uring_prep_splice(sqe2, io.fuse_ring.backing_fd,
        read_offset, fuse_io->pipe[1], -1, read_size,
        SPLICE_F_MOVE| SPLICE_F_FD_IN_FIXED | SPLICE_F_NONBLOCK);
    sqe2->flags |= IOSQE_FIXED_FILE | IOSQE_IO_LINK;

    io_uring_sqe *sqe3 = io.get_sqe();
    if(sqe3==nullptr)
        co_return -1;

    io_uring_prep_splice(sqe3, fuse_io->pipe[0],
        -1, io.fuse_ring.fd, -1, out_header->len,
        SPLICE_F_MOVE | SPLICE_F_FD_IN_FIXED | SPLICE_F_NONBLOCK);
    sqe3->flags |= IOSQE_FIXED_FILE;
    
    std::vector<int> rcs = co_await io.complete({sqe1, sqe2, sqe3});

    for(int rc: rcs)
    {
        if(rc<0)
            co_return -1;
    }

    if(rcs[0]<sizeof(fuse_out_header))
    {
        co_return -1;
    }

    if(rcs[1]<read_size)
    {
        co_return -1;
    }

    if(rcs[2]<out_header->len)
    {
        co_return -1;
    }

    co_return 0;
}

[[nodiscard]] fuse_io_context::io_uring_task<int> handle_write(fuse_io_context& io, fuse_io_context::FuseIoVal& fuse_io,
    char* rbytes_buf)
{
    fuse_in_header* fheader = reinterpret_cast<fuse_in_header*>(fuse_io->header_buf);
    uint64_t write_offset;
    uint32_t write_size;
    {
        fuse_write_in* write_in = reinterpret_cast<fuse_write_in*>(rbytes_buf);

        DBG_PRINT(std::cout << "write nodeid " << fheader->nodeid << " off: " << write_in->offset << " size: "<< write_in->size << std::endl);

        if(fheader->nodeid!=3)
        {
            fuse_out_header* out_header = reinterpret_cast<fuse_out_header*>(fuse_io->scratch_buf);
            out_header->unique = fheader->unique;
            out_header->error = -ENOENT;
            out_header->len = sizeof(fuse_out_header);
            co_return co_await send_reply(io, fuse_io);
        }

        write_offset = write_in->offset;
        write_size = write_in->size;

        /*if(write_offset + write_size > io.fuse_ring.backing_f_size)
        {
            write_size = io.fuse_ring.backing_f_size - write_offset;
            std::cout << "Writing less: " << write_size << std::endl;
        }*/

        if(fheader->len!=sizeof(fuse_in_header)+sizeof(fuse_write_in)+write_size)
        {
            fuse_out_header* out_header = reinterpret_cast<fuse_out_header*>(fuse_io->scratch_buf);
            out_header->unique = fheader->unique;
            out_header->error = -EINVAL;
            out_header->len = sizeof(fuse_out_header);
            co_return co_await send_reply(io, fuse_io);
        }
    }

    /*if(write_size<=4096)
    {
        io_uring_sqe* sqe1 = co_await io.get_sqe();
        io_uring_prep_read_fixed(sqe1, fuse_io->pipe[0], fuse_io->scratch_buf,
                write_size, 0, fuse_io->scratch_buf_idx);
        sqe1->flags |= IOSQE_FIXED_FILE | IOSQE_IO_LINK;

        io_uring_sqe* sqe2 = co_await io.get_sqe();
        io_uring_prep_write(sqe2, io.fuse_ring.backing_fd, fuse_io->scratch_buf,
                write_size, write_offset);
        sqe2->flags |= IOSQE_FIXED_FILE | IOSQE_IO_LINK;

        uint64_t header_unique = fheader->unique;

        fuse_out_header* out_header = reinterpret_cast<fuse_out_header*>(fuse_io->header_buf);
        out_header->error = 0;
        out_header->len = sizeof(fuse_out_header) + sizeof(fuse_write_out);
        out_header->unique = header_unique;

        fuse_write_out* write_out = reinterpret_cast<fuse_write_out*>(fuse_io->header_buf + sizeof(fuse_out_header));
        write_out->size = write_size;
        write_out->padding = 0;

        io_uring_sqe* sqe3 = co_await io.get_sqe();
        io_uring_prep_write(sqe3, fuse_io->pipe[1],
            fuse_io->header_buf, out_header->len,
            0);
        sqe3->flags |= IOSQE_FIXED_FILE | IOSQE_IO_LINK;
        
        io_uring_sqe* sqe4 = co_await io.get_sqe();
        io_uring_prep_splice(sqe4, fuse_io->pipe[0],
                -1, io.fuse_ring.fd, -1, out_header->len,
                SPLICE_F_MOVE| SPLICE_F_FD_IN_FIXED | SPLICE_F_NONBLOCK);
        sqe4->flags |= IOSQE_FIXED_FILE;

        std::vector<int> rcs = co_await io.complete({sqe1, sqe2, sqe3, sqe4});

        for(int rc: rcs)
        {
            if(rc<0)
                co_return -1;
        }

        co_return 0;
    }

    std::cout << "Splice ... write size " << write_size << std::endl;*/

    fuse_out_header* out_header = reinterpret_cast<fuse_out_header*>(fuse_io->scratch_buf);
    out_header->error = 0;
    out_header->len = sizeof(fuse_out_header) + sizeof(fuse_write_out);
    out_header->unique = fheader->unique;

    fuse_write_out* write_out = reinterpret_cast<fuse_write_out*>(fuse_io->scratch_buf + sizeof(fuse_out_header));
    write_out->size = write_size;
    write_out->padding = 0;

    io_uring_sqe* sqe1 = io.get_sqe(3);
    if(sqe1==nullptr)
        co_return -1;

    io_uring_prep_splice(sqe1, fuse_io->pipe[0],
        -1, io.fuse_ring.backing_fd, write_offset, write_size,
            SPLICE_F_MOVE| SPLICE_F_FD_IN_FIXED | SPLICE_F_NONBLOCK);            
    sqe1->flags |= IOSQE_FIXED_FILE | IOSQE_IO_LINK;

    io_uring_sqe* sqe2 = io.get_sqe();
    if(sqe2==nullptr)
        co_return -1;

    io_uring_prep_write_fixed(sqe2, fuse_io->pipe[1],
            fuse_io->scratch_buf, out_header->len,
            0, fuse_io->scratch_buf_idx);
    sqe2->flags |= IOSQE_FIXED_FILE | IOSQE_IO_LINK;

    io_uring_sqe *sqe3 = io.get_sqe();
    if(sqe3==nullptr)
        co_return -1;

    io_uring_prep_splice(sqe3, fuse_io->pipe[0],
        -1, io.fuse_ring.fd, -1, out_header->len,
        SPLICE_F_MOVE| SPLICE_F_FD_IN_FIXED | SPLICE_F_NONBLOCK);
    sqe3->flags |= IOSQE_FIXED_FILE;

    std::vector<int> rcs = co_await io.complete({sqe1, sqe2, sqe3});

    if(rcs[0]<0)
    {
        out_header->error = rcs[0];
        out_header->len = sizeof(fuse_out_header);
        co_return co_await send_reply(io, fuse_io);
    }

    if(rcs[0]<write_size)
    {
        write_out->size = rcs[0];
        co_return co_await send_reply(io, fuse_io);
    }

    if(rcs[1]<0 || rcs[2]<0 ||
        rcs[1]!=out_header->len || rcs[2]!=out_header->len)
    {
        co_return -1;
    }

    DBG_PRINT(std::cout << "FUSE_WRITE done" << std::endl);

    co_return 0;
}

[[nodiscard]] fuse_io_context::io_uring_task<int> handle_releasedir(fuse_io_context& io, fuse_io_context::FuseIoVal& fuse_io,
    char* rbytes_buf)
{
    fuse_in_header* fheader = reinterpret_cast<fuse_in_header*>(fuse_io->header_buf);
    fuse_release_in* release_in = reinterpret_cast<fuse_release_in*>(rbytes_buf);

    DBG_PRINT(std::cout << "releasedir nodeid " << fheader->nodeid << std::endl);

    fuse_out_header* out_header = reinterpret_cast<fuse_out_header*>(fuse_io->scratch_buf);
    out_header->error = 0;
    out_header->len = sizeof(fuse_out_header);
    out_header->unique = fheader->unique;
    
    co_return co_await send_reply(io, fuse_io);
}

[[nodiscard]] fuse_io_context::io_uring_task<int> handle_release(fuse_io_context& io, fuse_io_context::FuseIoVal& fuse_io,
    char* rbytes_buf)
{
    fuse_in_header* fheader = reinterpret_cast<fuse_in_header*>(fuse_io->header_buf);
    fuse_release_in* release_in = reinterpret_cast<fuse_release_in*>(rbytes_buf);

    DBG_PRINT(std::cout << "release nodeid " << fheader->nodeid << std::endl);

    fuse_out_header* out_header = reinterpret_cast<fuse_out_header*>(fuse_io->scratch_buf);
    out_header->error = 0;
    out_header->len = sizeof(fuse_out_header);
    out_header->unique = fheader->unique;
    
    co_return co_await send_reply(io, fuse_io);
}

void add_dir(std::vector<char>& buf, const std::string& name, size_t off, const struct stat& stbuf)
{
    size_t bsize = FUSE_DIRENT_ALIGN(FUSE_NAME_OFFSET + name.size());
    size_t orig_off = buf.size();
    buf.resize(buf.size()+bsize);
    fuse_dirent* dirent = reinterpret_cast<fuse_dirent*>(&buf[orig_off]);

    dirent->ino = stbuf.st_ino;
    dirent->namelen = name.size();
    dirent->off = off;
    dirent->type = (stbuf.st_mode & S_IFMT) >> 12;
    memcpy(dirent->name, name.data(), name.size());
    memset(dirent->name + name.size(), 0, bsize-FUSE_NAME_OFFSET-name.size());
}

[[nodiscard]] fuse_io_context::io_uring_task<int> handle_readdir(fuse_io_context& io, fuse_io_context::FuseIoVal& fuse_io,
    char* rbytes_buf)
{
    fuse_in_header* fheader = reinterpret_cast<fuse_in_header*>(fuse_io->header_buf);

    fuse_read_in* read_in = reinterpret_cast<fuse_read_in*>(rbytes_buf);

    DBG_PRINT(std::cout << "readdir nodeid " << fheader->nodeid << " offset: " << read_in->offset << " read_flags: " 
            << read_in->read_flags << " size: " << read_in->size << std::endl);

    std::vector<char> out_buf(sizeof(fuse_out_header));
    fuse_out_header* out_header = reinterpret_cast<fuse_out_header*>(out_buf.data());
    out_header->error = 0;
    out_header->unique = fheader->unique;

    if(read_in->offset==0)
    {
        struct stat stbuf = {};
        stbuf.st_mode = S_IFDIR | S_IRWXU | S_IRWXG | S_IRWXO;

        stbuf.st_ino = 2;
        add_dir(out_buf, ".", 1, stbuf);

        stbuf.st_ino = 3;
        add_dir(out_buf, "..", 2, stbuf);        
        
        stbuf.st_ino = 4;
        stbuf.st_size = io.fuse_ring.backing_f_size;
        stbuf.st_blocks = round_up<off_t>(stbuf.st_size, 512);
        add_dir(out_buf, "volume", 3, stbuf);       
    }

    out_header = reinterpret_cast<fuse_out_header*>(out_buf.data());
    out_header->len = out_buf.size();
    
    co_return co_await send_reply(io, fuse_io, out_buf);
}

fuse_io_context::io_uring_task<int> queue_fuse_read(fuse_io_context& io)
{
    fuse_io_context::FuseIoVal fuse_io = io.get_fuse_io();

    DBG_PRINT(std::cout << "queue_fuse_read" << std::endl);
    struct io_uring_sqe *sqe1 = io.get_sqe(2);
    struct io_uring_sqe *sqe2 = io.get_sqe();
    if(sqe1==nullptr || sqe2==nullptr)
        co_return -1;

    io_uring_prep_splice(sqe1, io.fuse_ring.fd, -1, fuse_io->pipe[1],
        -1, io.fuse_ring.max_bufsize, SPLICE_F_MOVE|SPLICE_F_NONBLOCK|SPLICE_F_FD_IN_FIXED);      
    sqe1->flags |= IOSQE_IO_HARDLINK | IOSQE_FIXED_FILE;

    io_uring_prep_read_fixed(sqe2, fuse_io->pipe[0], fuse_io->header_buf,
            sizeof(fuse_in_header) + sizeof(fuse_write_in), 0, fuse_io->header_buf_idx);
    sqe2->flags |= IOSQE_FIXED_FILE;

    auto [rbytes, init_read] = co_await io.complete(std::make_pair(sqe1, sqe2));

    if(rbytes<0 || rbytes<sizeof(fuse_in_header))
    {
        static bool erronce=true;
        if(erronce)
        {
            std::cerr << "Error reading from fuse rc=" << rbytes << std::endl;
            erronce=false;
        }
        co_return -1;
    }

    if(init_read>0 && init_read<sizeof(fuse_in_header))
    {
        do
        {
            io_uring_sqe *sqe = io.get_sqe();
            io_uring_prep_read_fixed(sqe, fuse_io->pipe[0], fuse_io->header_buf + init_read,
                sizeof(fuse_in_header)-init_read, 0, fuse_io->header_buf_idx);
            sqe->flags |= IOSQE_FIXED_FILE;

            int rc = co_await io.complete(sqe);

            if(rc<=0)
            {
                std::cerr << "Error reading fuse_in_header after short read res: " << rc << std::endl;
                co_return -1;
            }

            init_read+=rc;

        } while(init_read<sizeof(fuse_in_header));
    }
    

    if(init_read<0 || init_read<sizeof(fuse_in_header))
    {
        std::cerr << "Error reading fuse_in_header res: " << init_read << std::endl;
        co_return -1;
    }

    fuse_in_header* fheader = reinterpret_cast<fuse_in_header*>(fuse_io->header_buf);

    DBG_PRINT(std::cout << "## fheader opcode: "<< fheader->opcode << " unique: "<< fheader->unique << " rbytes: " << rbytes << " init_read: " << init_read << std::endl);

    size_t req_read_rbytes = 0;
    bool req_read_add_zero=false;
    bool req_allow_add_bytes=false;
    switch(fheader->opcode)
    {
        case FUSE_GETATTR:
            req_read_rbytes = sizeof(fuse_getattr_in);
            break;
        case FUSE_SETATTR:
            req_read_rbytes = sizeof(fuse_setattr_in);
            break;
        case FUSE_OPENDIR:
            req_read_rbytes = sizeof(fuse_open_in);
            break;
        case FUSE_READDIR:
            req_read_rbytes = sizeof(fuse_read_in);
            break;
        case FUSE_RELEASEDIR:
            req_read_rbytes = sizeof(fuse_release_in);
            break;
        case FUSE_LOOKUP:
            req_read_rbytes = rbytes - sizeof(fuse_in_header);
            req_read_add_zero=true;
            break;
        case FUSE_OPEN:
            req_read_rbytes = sizeof(fuse_open_in);
            break;
        case FUSE_READ:
            req_read_rbytes = sizeof(fuse_read_in);
            break;
        case FUSE_RELEASE:
            req_read_rbytes = sizeof(fuse_release_in);
            break;
        case FUSE_WRITE:
            req_read_rbytes = sizeof(fuse_write_in);
            req_allow_add_bytes=true;
            break;
        default:
            req_read_rbytes = rbytes - sizeof(fuse_in_header);
    }

    std::vector<char> rbytes_buf_d;
    char* rbytes_buf;
    if(req_read_rbytes>0)
    {
        if(!req_allow_add_bytes &&
            req_read_rbytes!=rbytes - sizeof(fuse_in_header))
        {
            co_return -1;
        }

        rbytes_buf = co_await read_rbytes(io, fuse_io, req_read_rbytes, 
            req_read_add_zero, init_read - sizeof(fuse_in_header), rbytes_buf_d);
        if(rbytes_buf==nullptr)
        {
            co_return -1;
        }
    }

/*#undef DBG_PRINT
#define DBG_PRINT(x) x*/
    int rc;
    switch(fheader->opcode)
    {
        case FUSE_GETATTR:
            DBG_PRINT(std::cout << "FUSE_GETATTR" << std::endl);
            rc = co_await handle_getattr(io, fuse_io, rbytes_buf);
            break;
        case FUSE_SETATTR:
            DBG_PRINT(std::cout << "FUSE_SETATTR" << std::endl);
            rc = co_await handle_setattr(io, fuse_io, rbytes_buf);
            break;
        case FUSE_OPENDIR:
            DBG_PRINT(std::cout << "FUSE_OPENDIR" << std::endl);
            rc = co_await handle_opendir(io, fuse_io, rbytes_buf);
            break;
        case FUSE_READDIR:
            DBG_PRINT(std::cout << "FUSE_READDIR" << std::endl);
            rc = co_await handle_readdir(io, fuse_io, rbytes_buf);
            break;
        case FUSE_RELEASEDIR:
            DBG_PRINT(std::cout << "FUSE_RELEASEDIR" << std::endl);
            rc = co_await handle_releasedir(io, fuse_io, rbytes_buf);
            break;
        case FUSE_LOOKUP:
            DBG_PRINT(std::cout << "FUSE_LOOKUP" << std::endl);
            rc = co_await handle_lookup(io, fuse_io, rbytes_buf);
            break;
        case FUSE_OPEN:
            DBG_PRINT(std::cout << "FUSE_OPEN" << std::endl);
            rc = co_await handle_open(io, fuse_io, rbytes_buf);
            break;
        case FUSE_READ:
            DBG_PRINT(std::cout << "FUSE_READ" << std::endl);
            rc = co_await handle_read(io, fuse_io, rbytes_buf);
            break;
        case FUSE_RELEASE:
            DBG_PRINT(std::cout << "FUSE_RELEASE" << std::endl);
            rc = co_await handle_release(io, fuse_io, rbytes_buf);
            break;
        case FUSE_WRITE:
            DBG_PRINT(std::cout << "FUSE_WRITE" << std::endl);
            rc = co_await handle_write(io, fuse_io, rbytes_buf);
            break;
        default:
            DBG_PRINT(std::cout << "## Unhandled opcode: " << fheader->opcode << std::endl);
            rc = co_await handle_unknown(io, fuse_io);
            break;
    }
/*#undef DBG_PRINT
#define DBG_PRINT(x)*/

    DBG_PRINT(std::cout << "## handle fuse done" << std::endl);
    co_return rc;
}

int fuseuring_main(int backing_fd, const std::string& mountpoint, int max_fuse_ios, 
    int max_background, int congestion_threshold, size_t n_threads)
{
    umount(mountpoint.c_str());

    struct stat stbuf;
    int rc = stat(mountpoint.c_str(), &stbuf);
    if(rc!=0)
    {
        perror(("Error stat() mountpoint \""+mountpoint+"\". ").c_str());
        return 1;
    }

    int fuse_fd = open("/dev/fuse", O_RDWR | O_CLOEXEC);
    if(fuse_fd==-1)
    {
        perror("Error opening /dev/fuse.");
        return 2;
    }

    char rootmode[10];
    snprintf(rootmode, sizeof(rootmode), "%o", stbuf.st_mode & S_IFMT);
    std::string mount_opts = "fd="+std::to_string(fuse_fd)+",rootmode="+std::string(rootmode)+",user_id=0,group_id=0,default_permissions,allow_other";

    rc = mount("tclouddrive", mountpoint.c_str(), "fuse", MS_NOSUID | MS_NODEV|MS_NOATIME|MS_NOEXEC, mount_opts.c_str());

    if(rc!=0)
    {
        perror("Error mounting fuse file system.");
        return 3;
    }

    std::vector<char> init_buf(8192);
    rc = read(fuse_fd, init_buf.data(), init_buf.size());

    if(rc<=0)
    {
        perror(("Err fuse init rc="+std::to_string(rc)).c_str());
        return 4;
    }

    struct InitInMsg
    {
        fuse_in_header header;
        fuse_init_in init_in;
    };

    InitInMsg* init_in = reinterpret_cast<struct InitInMsg*>(init_buf.data());

    if(init_in->header.opcode != FUSE_INIT)
    {
        std::cerr << "Unexpected opcode during init" << std::endl;
        return 5;
    }

    if(init_in->header.len!=sizeof(InitInMsg))
    {
        std::cerr << "Unexpected length during init" << std::endl;
        return 5;
    }

    if(init_in->init_in.major<FUSE_KERNEL_VERSION)
    {
        std::cerr << "Unsupported fuse major version " << init_in->init_in.major << std::endl;
        return 5;
    }

    struct InitOutMsg
    {
        fuse_out_header header;
        fuse_init_out init_out;
    };
    InitOutMsg init_out;
    init_out.header.error = 0;
    init_out.header.len = sizeof(InitOutMsg);
    init_out.header.unique = init_in->header.unique;
    init_out.init_out.major = FUSE_KERNEL_VERSION;
    init_out.init_out.minor =FUSE_KERNEL_MINOR_VERSION;

    if(init_in->init_in.major>FUSE_KERNEL_VERSION)
    {
        if(write(fuse_fd, &init_out, sizeof(init_out))!=sizeof(init_out))
        {
            perror("Error writing ver reply");
            return 6;
        }

        int rc = read(fuse_fd, init_buf.data(), init_buf.size());

        if(rc<=0)
        {
            perror("Err fuse init 2");
            return 7;
        }
    }

    init_out.init_out.max_readahead = init_in->init_in.max_readahead;
    if( !(init_in->init_in.flags & FUSE_ASYNC_READ)
        || !(init_in->init_in.flags & FUSE_PARALLEL_DIROPS)
        || !(init_in->init_in.flags & FUSE_AUTO_INVAL_DATA)
        || !(init_in->init_in.flags & FUSE_HANDLE_KILLPRIV)
        || !(init_in->init_in.flags & FUSE_ASYNC_DIO)
        || !(init_in->init_in.flags & FUSE_IOCTL_DIR)
        || !(init_in->init_in.flags & FUSE_ATOMIC_O_TRUNC)
        || !(init_in->init_in.flags & FUSE_SPLICE_READ)
        || !(init_in->init_in.flags & FUSE_SPLICE_WRITE)
        || !(init_in->init_in.flags & FUSE_MAX_PAGES) 
        || !(init_in->init_in.flags & FUSE_WRITEBACK_CACHE) 
        || !(init_in->init_in.flags & FUSE_EXPORT_SUPPORT) 
        || !(init_in->init_in.flags & FUSE_SPLICE_MOVE) )
    {
        std::cerr << "Linux kernel is missing required fuse capabilities." << std::endl;
        return 8;    
    }

    init_out.init_out.flags = FUSE_MAX_PAGES |FUSE_PARALLEL_DIROPS
        | FUSE_BIG_WRITES |FUSE_ASYNC_READ | FUSE_AUTO_INVAL_DATA
        | FUSE_HANDLE_KILLPRIV | FUSE_ASYNC_DIO | FUSE_IOCTL_DIR
        | FUSE_ATOMIC_O_TRUNC | FUSE_SPLICE_READ | FUSE_SPLICE_WRITE
        | FUSE_MAX_PAGES | FUSE_WRITEBACK_CACHE | FUSE_EXPORT_SUPPORT
        | FUSE_SPLICE_MOVE;


    init_out.init_out.max_background = max_background;
    init_out.init_out.congestion_threshold = congestion_threshold;
    init_out.init_out.max_pages = static_cast<uint16_t>(fuse_max_pages);
    init_out.init_out.time_gran = 1;
    size_t max_write = getpagesize()*fuse_max_pages;
    init_out.init_out.max_write = max_write;
    
    if(write(fuse_fd, &init_out, sizeof(init_out))!=sizeof(init_out))
    {
        perror("Error writing init reply");
        return 9;
    }

    if(n_threads<=1)
    {
        return fuseuring_run(max_background, max_write, backing_fd, fuse_fd, nullptr, 0);
    }
    else
    {
        struct io_uring fuse_uring;

        int rc = io_uring_queue_init(std::max(100, max_fuse_ios*2), &fuse_uring, 0);

        if(rc<0)
        {
            perror("Error setting up io_uring.");
            return 10;
        }

        int thread_rc=0;
        std::vector<std::thread> threads;
        for(size_t i=0;i<n_threads;++i)
        {
            threads.push_back(std::thread( [max_fuse_ios, 
                    max_write, backing_fd, fuse_fd, &thread_rc, i, &fuse_uring] () {

                int session_fd = open("/dev/fuse", O_RDWR | O_CLOEXEC);
                if(session_fd==-1)
                {
                    perror("Error opening /dev/fuse -2.");
                    thread_rc=2;
                    return;
                }

                int rc = ioctl(session_fd, FUSE_DEV_IOC_CLONE, &fuse_fd);
                if(rc==-1)
                {
                    perror("Error cloning fuse fd");
                    thread_rc=3;
                }

                rc = fuseuring_run(max_fuse_ios, 
                        max_write, backing_fd, session_fd,
                        i==0 ? &fuse_uring : nullptr,
                        i==0 ? 0 : fuse_uring.ring_fd);
                if(rc!=0)
                    thread_rc=rc;
            }));
        }

        for(auto& thread: threads)        
        {
            thread.join();
        }
        return thread_rc;
    }
}

int fuseuring_run(int max_fuse_ios, size_t max_write, 
    int backing_fd, int fuse_fd, struct io_uring* fuse_uring, int uring_wq_fd)
{
    struct io_uring fuse_uring_local;

    if(fuse_uring==nullptr)
    {
        struct io_uring_params p = {};

        //p.flags = IORING_SETUP_SQPOLL;
        if(uring_wq_fd!=0)
        {
            p.flags = IORING_SETUP_ATTACH_WQ;
        }
        p.wq_fd = uring_wq_fd;

        int rc = io_uring_queue_init_params(std::max(100, max_fuse_ios*2), &fuse_uring_local, &p);//IORING_SETUP_SQPOLL);

        if(rc<0)
        {
            perror("Error setting up io_uring.");
            return 10;
        }

        fuse_uring = &fuse_uring_local;
    }
    
    std::vector<int> fixed_fds;
    fixed_fds.push_back(fuse_fd);
    int fuse_fixed_fd = 0;
    std::vector<struct iovec> reg_buffers;

    fuse_io_context::FuseRing fuse_ring;
    fuse_ring.fd = fuse_fixed_fd;
    fuse_ring.backing_fd = fixed_fds.size();
    fixed_fds.push_back(backing_fd);
    fuse_ring.backing_fd_orig = backing_fd;

    size_t max_bufsize = max_write + sizeof(fuse_in_header) + sizeof(fuse_write_in);

    std::vector<char> header_buf_v(header_buf_size*max_fuse_ios);
    char* header_buf = header_buf_v.data();

    struct iovec iov;
    iov.iov_base = header_buf;
    iov.iov_len = header_buf_v.size();
    size_t header_buf_idx = reg_buffers.size();
    reg_buffers.push_back(iov);

    std::vector<char> scratch_buf_v(scratch_buf_size*max_fuse_ios);
    char* scratch_buf = scratch_buf_v.data();
    iov.iov_base = scratch_buf;
    iov.iov_len = scratch_buf_v.size();
    size_t scratch_buf_idx = reg_buffers.size();
    reg_buffers.push_back(iov);

    std::vector<int> pipe_fds;

    for(size_t i=0;i<max_fuse_ios;++i)
    {
        std::unique_ptr<fuse_io_context::FuseIo> new_io = std::make_unique<fuse_io_context::FuseIo>();
        int rc = pipe2(new_io->pipe, O_CLOEXEC|O_NONBLOCK);
        if(rc!=0)
        {
            perror("Error creating pipe.");
            return 11;
        }

        rc = fcntl(new_io->pipe[0], F_SETPIPE_SZ, max_bufsize);
        if(rc<0)
        {
            perror(("Error setting pipe size to "+std::to_string(max_bufsize)+".").c_str());
            return 12;
        }

        pipe_fds.push_back(new_io->pipe[0]);
        pipe_fds.push_back(new_io->pipe[1]);

        fixed_fds.push_back(new_io->pipe[0]);
        new_io->pipe[0] = fixed_fds.size()-1;
        fixed_fds.push_back(new_io->pipe[1]);
        new_io->pipe[1] = fixed_fds.size()-1;

        new_io->header_buf = header_buf;
        header_buf+=header_buf_size;
        new_io->header_buf_idx = header_buf_idx;

        new_io->scratch_buf = scratch_buf;
        scratch_buf+=scratch_buf_size;
        new_io->scratch_buf_idx = scratch_buf_idx;

        fuse_ring.ios.push_back(std::move(new_io));
    }

    int rc = io_uring_register_files(fuse_uring, &fixed_fds[0], fixed_fds.size());
    if(rc<0)
    {
        perror("Error registering fuse io_uring files.");
        return 13;
    }

    rc = io_uring_register_buffers(fuse_uring, &reg_buffers[0], reg_buffers.size());
    if(rc<0)
    {
        perror("Error registering fuse io_uring buffers.");
        return 14;
    }
    
    fuse_ring.ring = fuse_uring;
    fuse_ring.ring_submit = false;
    fuse_ring.max_bufsize = max_bufsize;

    struct stat bst;
    if(fstat(backing_fd, &bst)!=0)
    {
        perror("Error getting backing file info");
        return 15;
    }

    fuse_ring.backing_f_size = bst.st_size;

    std::cout << "Running..." << std::endl;
    fuse_io_context service(std::move(fuse_ring));
    rc = service.run(queue_fuse_read);

    io_uring_unregister_buffers(fuse_uring);
    io_uring_unregister_files(fuse_uring);

    io_uring_queue_exit(fuse_uring);

    for(int p: pipe_fds)
    {
        close(p);
    }

    fuse_io_context::clear_malloc_cache();

    return rc;
}
