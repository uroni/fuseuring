// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) Martin Raiber
#pragma once
#include <coroutine>
#include <vector>
#include <liburing.h>
#include <stack>
#include <utility>
#include <assert.h>
#include <iostream>

#define DBG_PRINT(x)

/*
for clang and libc++
namespace std
{
    template<typename T = void>
    using coroutine_handle = std::experimental::coroutine_handle<T>;

    using suspend_never = std::experimental::suspend_never;

    using suspend_always = std::experimental::suspend_always;
}*/

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

    struct IoUringSqeAwaiter;
    IoUringSqeAwaiter* sqe_awaiters = nullptr;

    struct IoUringSqeAwaiter
    {
        IoUringSqeAwaiter(fuse_io_service& io, struct io_uring* uring)
         : io(io), next(nullptr)
        {
            sqe = io_uring_get_sqe(uring);
        }

        IoUringSqeAwaiter(IoUringSqeAwaiter const&) = delete;
	    IoUringSqeAwaiter(IoUringSqeAwaiter&& other) = delete;
	    IoUringSqeAwaiter& operator=(IoUringSqeAwaiter&&) = delete;
	    IoUringSqeAwaiter& operator=(IoUringSqeAwaiter const&) = delete;

        bool await_ready() const noexcept
        {
            return sqe!=nullptr;
        }

        void await_suspend(std::coroutine_handle<> p_awaiter) noexcept
        {
            DBG_PRINT(std::cout << "Await suspend sqe "<< handle_v(p_awaiter) << std::endl);
            awaiter = p_awaiter;
            if(io.sqe_awaiters!=nullptr)
            {
                next = io.sqe_awaiters;    
            }
            io.sqe_awaiters = this;
        }

        struct io_uring_sqe* await_resume() const noexcept
        {
            return sqe;
        }

        fuse_io_service& io;
        std::coroutine_handle<> awaiter;
        IoUringSqeAwaiter* next;
        struct io_uring_sqe *sqe;              
    };

    [[nodiscard]] auto get_sqe()
    {
        fuse_ring.ring_submit=true;
        return IoUringSqeAwaiter(*this, fuse_ring.ring);
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
            res_state = e_res_state::Res;
            res = v;
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

    struct SFuseIo
    {
        char type;
        int pipe[2];
        char* header_buf;
        size_t header_buf_idx;
        char* scratch_buf;
        size_t scratch_buf_idx;
    };

    struct FuseRing
    {
        std::stack<SFuseIo*> ios;
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

    typedef fuse_io_service::io_uring_task<int> (*queue_fuse_read_t)(fuse_io_service& io);

    int run(queue_fuse_read_t queue_read);

    SFuseIo* get_fuse_io()
    {
        fuse_io_service::SFuseIo* fuse_io = fuse_ring.ios.top();
        fuse_ring.ios.pop();
        return fuse_io;
    }

    void release_fuse_io(SFuseIo* fuse_io)
    {
        fuse_ring.ios.push(fuse_io);
    }

private:
    int fuseuring_handle_cqe(struct io_uring_cqe *cqe);
    int fuseuring_submit(bool block);
    int run_sqe_awaiters();
};