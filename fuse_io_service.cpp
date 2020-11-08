// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) Martin Raiber
#include "fuse_io_service.h"
#include <liburing.h>
#include <iostream>


fuse_io_service::fuse_io_service(FuseRing fuse_ring)
 : fuse_ring(std::move(fuse_ring)), last_rc(0)
{
}

int fuse_io_service::fuseuring_handle_cqe(struct io_uring_cqe *cqe)
{
    if(cqe->user_data==0)
    {
        DBG_PRINT(std::cerr << "Cqe no user_data" << std::endl);
        return 0;
    }

    IoUringAwaiter<int>::IoUringAwaiterRes* res = reinterpret_cast<IoUringAwaiter<int>::IoUringAwaiterRes*>(cqe->user_data);
    res->res = cqe->res;
    DBG_PRINT(std::cout << "Cqe res "<< cqe->res << std::endl);
    --res->gres->tocomplete;
    if(res->gres->tocomplete==0)
    {
        DBG_PRINT(std::cerr << "Resume cqe..." << std::endl);
        res->gres->awaiter.resume();
    }

    return 0;    
}

int fuse_io_service::fuseuring_submit(bool block)
{
    if(fuse_ring.ring_submit)
    {
        int rc;
        if(block)
            rc = io_uring_submit_and_wait(fuse_ring.ring, 1);
        else
            rc = io_uring_submit(fuse_ring.ring);

        if(rc<0)
        {
            perror("Error submitting to fuse io_uring.");
            return 18;
        }
        fuse_ring.ring_submit=false;
    }
    else if(block)
    {
        int rc = io_uring_submit_and_wait(fuse_ring.ring, 1);
        if(rc<0)
        {
            perror("Error submitting to fuse io_uring (2).");
            return 18;
        }
    }
    return 0;
}

int fuse_io_service::run(queue_fuse_read_t queue_read)
{
    fuse_ring.ring_submit = false;

    while(true)
    {
        while(!fuse_ring.ios.empty())
        {
            queue_read_set_rc(queue_read);
        }

        if(int rc; (rc=fuseuring_submit(true))!=0)
            return rc;


        unsigned head;
        unsigned count=0;
        struct io_uring_cqe *cqe;
        io_uring_for_each_cqe(fuse_ring.ring, head, cqe)
        {
            int rc = fuseuring_handle_cqe(cqe);
            if(rc<0)
            {
                std::cerr << "Error handling cqe rc=" << rc << std::endl;
                return 17;
            }
            ++count;
        }
        io_uring_cq_advance(fuse_ring.ring, count);

        if(last_rc)
        {
            std::cerr << "Task failed rc=" << last_rc << ". Shutting down." << std::endl;
            return 19;
        }
    }
}

fuse_io_service::io_uring_task_discard<int> fuse_io_service::queue_read_set_rc(queue_fuse_read_t queue_read)
{
    int rc = co_await queue_read(*this);
    if(rc!=0)
    {
        last_rc=rc;
    }
    co_return rc;
}