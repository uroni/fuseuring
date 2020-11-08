// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) Martin Raiber
#pragma once
#include <coroutine>
#include <vector>
#include <liburing.h>
#include <utility>
#include <assert.h>
#include <iostream>
#include <unistd.h>
#include <memory>

#define DBG_PRINT(x)

/*
//for clang and libc++
namespace std
{
    template<typename T = void>
    using coroutine_handle = std::experimental::coroutine_handle<T>;

    using suspend_never = std::experimental::suspend_never;

    using suspend_always = std::experimental::suspend_always;
}
*/

static uint64_t handle_v(std::coroutine_handle<> p_awaiter)
{
    return (uint64_t)*((uint64_t*)&p_awaiter);
}

struct fuse_io_service
{
    template<typename T>
    struct IoUringAwaiter
    {
        struct IoUringAwaiterGlobalRes
        {
            size_t tocomplete;
            std::coroutine_handle<> awaiter;
        };

        struct IoUringAwaiterRes
        {
            IoUringAwaiterRes() noexcept
            : res(-1) {}

            int res;
            IoUringAwaiterGlobalRes* gres;
        };

        IoUringAwaiter(std::vector<io_uring_sqe*> sqes) noexcept
        {
            awaiter_res.resize(sqes.size());
            global_res.tocomplete = sqes.size();
            for(size_t i=0;i<sqes.size();++i)
            {
                awaiter_res[i].gres = &global_res;
                sqes[i]->user_data = reinterpret_cast<uint64_t>(&awaiter_res[i]);
            }
        }        

        IoUringAwaiter(IoUringAwaiter const&) = delete;
	    IoUringAwaiter(IoUringAwaiter&& other) = delete;
	    IoUringAwaiter& operator=(IoUringAwaiter&&) = delete;
	    IoUringAwaiter& operator=(IoUringAwaiter const&) = delete;

        bool await_ready() const noexcept
        {
            return false;
        }

        void await_suspend(std::coroutine_handle<> p_awaiter) noexcept
        {
            DBG_PRINT(std::cout << "Await suspend io "<< handle_v(p_awaiter) << std::endl);
            global_res.awaiter = p_awaiter;           
        }

        template<typename U = T, std::enable_if_t<std::is_same<U, std::vector<io_uring_sqe*> >::value, int> = 0>
        std::vector<int> await_resume() const noexcept
        {
            std::vector<int> res;
            for(auto& sr: awaiter_res)
            {
                res.push_back(sr.res);
            }
            return res;
        }

        template<typename U = T, std::enable_if_t<std::is_same<U, io_uring_sqe*>::value, int> = 0>
        int await_resume() const noexcept
        {
            return awaiter_res[0].res;
        }

        template<typename U = T, std::enable_if_t<std::is_same<U, std::pair<io_uring_sqe*, io_uring_sqe*> >::value, int> = 0>
        std::pair<int, int> await_resume() const noexcept
        {
            return std::make_pair(awaiter_res[0].res, awaiter_res[1].res);
        }

    private:
        IoUringAwaiterGlobalRes global_res;
        std::vector<IoUringAwaiterRes> awaiter_res;         
    };

    [[nodiscard]] auto complete(std::vector<io_uring_sqe*> sqes)
    {
        return IoUringAwaiter<std::vector<io_uring_sqe*> >(sqes);
    }

    [[nodiscard]] auto complete(io_uring_sqe* sqe)
    {
        return IoUringAwaiter<io_uring_sqe*>({sqe});
    }

    [[nodiscard]] auto complete(std::pair<io_uring_sqe*, io_uring_sqe*> sqes)
    {
        return IoUringAwaiter<std::pair<io_uring_sqe*, io_uring_sqe*> >({sqes.first, sqes.second});
    }

    io_uring_sqe* get_sqe() noexcept
    {
        fuse_ring.ring_submit=true;
        auto ret = io_uring_get_sqe(fuse_ring.ring);
        if(ret==nullptr)
        {
            /* Needs newer Linux 5.10
            int rc = io_uring_sqring_wait(fuse_ring.ring);
            if(rc<0)
            {
                return nullptr;
            }
            else if(rc==0)
            {*/
                int rc = io_uring_submit(fuse_ring.ring);
                if(rc<0)
                {
                    perror("io_uring_submit failed in get_sqe");
                    return nullptr;
                }

                do
                { 
                    ret = io_uring_get_sqe(fuse_ring.ring);
                } while (ret==nullptr);
            //}
        }
        return ret;
    }

    template<typename T>
    struct io_uring_promise_type
    {
        using promise_type = io_uring_promise_type<T>;
        using handle = std::coroutine_handle<promise_type>;

        enum class e_res_state
        {
            Init,
            Detached,
            Res
        };

        io_uring_promise_type() 
            : res_state(e_res_state::Init) {}

        auto get_return_object() 
        {
            return io_uring_task<T>{handle::from_promise(*this)};
        }

        void return_value(T v)
        {
            if(res_state!=e_res_state::Detached)
            {
                res_state = e_res_state::Res;
                res = v;
            }
        }

        auto initial_suspend() { 
            return std::suspend_never{}; 
        }
    
        auto final_suspend() noexcept { 
            struct final_awaiter  : std::suspend_always
            {
                final_awaiter(promise_type* promise)
                    :promise(promise) {}

                void await_suspend(std::coroutine_handle<> p_awaiter) const noexcept
                {
                    if(promise->res_state==e_res_state::Detached)
                    {
                        DBG_PRINT(std::cout << "promise final detached" << std::endl);
                        if(promise->awaiter)
                            promise->awaiter.destroy();
                        handle::from_promise(*promise).destroy();
                    }
                    else if(promise->awaiter)
                    {
                        DBG_PRINT(std::cout << "promise final await resume" << std::endl);
                        promise->awaiter.resume();
                    }
                    else
                    {
                        DBG_PRINT(std::cout << "promise final no awaiter" << std::endl);
                    }
                    
                }

            private:
                promise_type* promise;
            };
            return final_awaiter(this);
        }

        void unhandled_exception()
        {
            abort();
        }

        std::coroutine_handle<> awaiter;
        
        e_res_state res_state;
        T res;
    };

    template<typename T>
    struct [[nodiscard]] io_uring_task
    {
        using promise_type = io_uring_promise_type<T>;
        using handle = std::coroutine_handle<promise_type>;

        io_uring_task(io_uring_task const&) = delete;

	    io_uring_task(io_uring_task&& other) noexcept
            : coro_h(std::exchange(other.coro_h, {}))
        {

        }

	    io_uring_task& operator=(io_uring_task&&) = delete;
	    io_uring_task& operator=(io_uring_task const&) = delete;

        io_uring_task(handle h) noexcept
         : coro_h(h)
        {

        }

        ~io_uring_task() noexcept
        {
            if(coro_h)
            {
                if(!coro_h.done())
                {
                    DBG_PRINT(std::cout << "Detach" << std::endl);
                    coro_h.promise().res_state = promise_type::e_res_state::Detached;
                }
                else
                {
                    DBG_PRINT(std::cout << "Destroy" << std::endl);
                    coro_h.destroy();
                }
            }
        }

        bool has_res() const noexcept
        {
            return coro_h.promise().res_state == promise_type::e_res_state::Res;
        }

        T res() const noexcept
        {
            assert(has_res());
            return coro_h.promise().res;
        }

        bool await_ready() const noexcept
        {
            bool r = has_res();
            if(r)
            {
                DBG_PRINT(std::cout << "Await is ready" << std::endl);
            }
            else
            {
                DBG_PRINT(std::cout << "Await is not ready" << std::endl);
            }
            
            return r;
        }

        template<typename U>
        void await_suspend(std::coroutine_handle<io_uring_promise_type<U> > p_awaiter) noexcept
        {
            DBG_PRINT(std::cout << "Await task " << (uint64_t)this << " suspend "<< handle_v(p_awaiter) << " prev " << handle_v(coro_h.promise().awaiter) << std::endl);
            coro_h.promise().awaiter = p_awaiter;
        }

        T await_resume() const noexcept
        {
            return res();
        }

    private:
        handle coro_h;
    };

    struct FuseIo
    {
        int pipe[2];
        char* header_buf;
        size_t header_buf_idx;
        char* scratch_buf;
        size_t scratch_buf_idx;
    };

    struct FuseIoVal
    {
        FuseIoVal(fuse_io_service& io_service,
            std::unique_ptr<FuseIo> fuse_io)
         : io_service(io_service),
            fuse_io(std::move(fuse_io))
        {

        }

        ~FuseIoVal()
        {
            io_service.release_fuse_io(std::move(fuse_io));
        }

        FuseIo& get() const noexcept
        {
            return *fuse_io.get();
        }

        FuseIo* operator->() const noexcept
        {
            return fuse_io.get();
        }

    private:
        fuse_io_service& io_service;
        std::unique_ptr<FuseIo> fuse_io;
    };

    struct FuseRing
    {
        FuseRing()
            : ring(nullptr), fd(-1), ring_submit(false),
                max_bufsize(1*1024*1024), backing_fd(-1),
                backing_fd_orig(-1), backing_f_size(0)
                {}

        FuseRing(FuseRing&&) = default;
        FuseRing(FuseRing const&) = delete;
	    FuseRing& operator=(FuseRing&&) = delete;
	    FuseRing& operator=(FuseRing const&) = delete;

        std::vector<std::unique_ptr<FuseIo> > ios;
        struct io_uring* ring;
        int fd;
        bool ring_submit;
        size_t max_bufsize;
        int backing_fd;
        int backing_fd_orig;
        uint64_t backing_f_size;
    };

    FuseRing fuse_ring;

    fuse_io_service(FuseRing fuse_ring);
    fuse_io_service(fuse_io_service const&) = delete;
	fuse_io_service(fuse_io_service&& other) = delete;
	fuse_io_service& operator=(fuse_io_service&&) = delete;
	fuse_io_service& operator=(fuse_io_service const&) = delete;

    typedef fuse_io_service::io_uring_task<int> (*queue_fuse_read_t)(fuse_io_service& io);

    int run(queue_fuse_read_t queue_read);

    FuseIoVal get_fuse_io()
    {
        std::unique_ptr<FuseIo> fuse_io = std::move(fuse_ring.ios.back());
        fuse_ring.ios.pop_back();
        return FuseIoVal(*this, std::move(fuse_io));
    }

    void release_fuse_io(std::unique_ptr<FuseIo> fuse_io)
    {
        fuse_ring.ios.push_back(std::move(fuse_io));
    }

private:

    template<typename T>
    struct io_uring_task_discard : io_uring_task<T>
    {
        io_uring_task_discard(io_uring_task<T>&& other) noexcept
            : io_uring_task<T>(std::move(other))
        {
        }
    };

    fuse_io_service::io_uring_task_discard<int> queue_read_set_rc(queue_fuse_read_t queue_read);

    int fuseuring_handle_cqe(struct io_uring_cqe *cqe);
    int fuseuring_submit(bool block);
    
    int last_rc;
};