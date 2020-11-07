// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) Martin Raiber
#include "fuse_io_service.h"
#include <liburing.h>
#include <iostream>


fuse_io_service::fuse_io_service(FuseRing fuse_ring)
 : fuse_ring(fuse_ring)
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
    return 0;
}

int fuse_io_service::run_sqe_awaiters()
{
    if(sqe_awaiters!=nullptr)
    {
        if(int rc; (rc=fuseuring_submit(false))!=0)
            return rc;
    }

    while(sqe_awaiters!=nullptr)
    {
        struct io_uring_sqe *sqe = io_uring_get_sqe(fuse_ring.ring);

        if(sqe)
        {
            IoUringSqeAwaiter* curr = sqe_awaiters;
            sqe_awaiters = curr->next;
            curr->sqe = sqe;
            curr->awaiter.resume();
        }            
        else
        {
            break;
        }
    }

    return 0;
}

int fuse_io_service::run(queue_fuse_read_t queue_read)
{
    fuse_ring.ring_submit = false;

    std::vector<io_uring_task<int> > tasks;
    while(true)
    {
        if(int rc; (rc=run_sqe_awaiters())!=0)
            return rc;

        while(!fuse_ring.ios.empty())
        {
            io_uring_task task = queue_read(*this);
            
            if(task.has_res())
            {
                int rc = task.res();

                if(rc<0)
                {
                    std::cerr << "Error after running task (1): " << rc << std::endl;
                    return 20;
                }
            }
            else
            {
                tasks.push_back(std::move(task));
            }
        }

        if(int rc; (rc=run_sqe_awaiters())!=0)
            return rc;

        if(int rc; (rc=fuseuring_submit(true))!=0)
            return rc;

        int nr_comp = 0;
        while(true)
        {
            struct io_uring_cqe *cqe;
            if(nr_comp==0)
            {
                int rc = io_uring_wait_cqe(fuse_ring.ring, &cqe);
                if(rc<0)
                {
                    perror("Waiting for fuse iouring cqe failed.");
                    return 16;
                }
            }
            else
            {
                int rc = io_uring_peek_cqe(fuse_ring.ring, &cqe);
                if(rc!=0)
                {
                    break;
                }
            }           

            int rc = fuseuring_handle_cqe(cqe);
            if(rc<0)
            {
                std::cerr << "Error handling cqe rc=" << rc << std::endl;
                return 17;
            }

            io_uring_cqe_seen(fuse_ring.ring, cqe);

            ++nr_comp;
        }

        std::vector<io_uring_task<int> > new_tasks;
        for(auto& task: tasks)
        {
            if(task.has_res())
            {
                int rc = task.res();

                if(rc<0)
                {
                    std::cerr << "Error running task (2). rc: " << rc << std::endl;
                    return 18;                    
                }
            }
            else
            {
                new_tasks.push_back(std::move(task));
            }            
        }

        std::swap(tasks, new_tasks);
    }
}