/*  __    ___         __    ___   ___   __
 * / /`  / / \  ___  / /`_ | |_) | |_) / /`
 * \_\_, \_\_/ |___| \_\_/ |_| \ |_|   \_\_,
 *
 * MIT License
 *
 * Copyright (c) 2021 Donald-Rupin
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 *  @file co_grpc.cpp
 *
 */

#ifndef CO_GRCP_HPP_
#define CO_GRCP_HPP_

#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <deque>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace grpc {
    class Server;
    class ServerCompletionQueue;
    class ServerBuilder;
    class ServerContext;
}   // namespace grpc

namespace co_grpc {

    template <typename Service, typename Executer>
    class grpc_service {

        public:

            class request {

                    friend class grpc_service;

                public:

                    request(grpc_service& _service)
                        : service_(_service), next_(nullptr), state_(kNew)
                    { }

                    virtual ~request(){};

                    void
                    proceed()
                    {
                        if (state_ != kDestory)
                        {
                            if (state_ == kNew)
                            {
                                clone();
                                state_ = kProcessing;
                            }

                            process();
                        }
                        else
                        {
                            destroy();
                        }
                    }

                    inline void
                    complete() noexcept
                    {
                        state_ = kDestory;
                    }

                    inline grpc_service&
                    server() noexcept
                    {
                        return service_;
                    }

                    inline grpc::ServerContext&
                    context() noexcept
                    {
                        return ctx_;
                    }

                private:

                    virtual void
                    process() = 0;

                    virtual void
                    error(){
                        destroy();
                    };

                    virtual void
                    clone() = 0;

                    virtual void
                    destroy()
                    {
                        delete this;
                    }

                    grpc_service& service_;

                    grpc::ServerContext ctx_;

                    request* next_;

                    enum State {
                        kNew,
                        kProcessing,
                        kDestory
                    };

                    State state_;
            };

            template <typename... Args>
            grpc_service(Args&&... _args)
                : executer_(std::forward<Args>(_args)...), writer_(nullptr), reader_(nullptr)
            { }

            ~grpc_service() = default;

            template <typename Creds>
            void
            build(std::string_view _address, Creds&& cred)
            {
                grpc::ServerBuilder builder;
                builder.AddListeningPort(_address.data(), std::forward<Creds>(cred));
                builder.RegisterService(&service_);
                cq_     = builder.AddCompletionQueue();
                server_ = builder.BuildAndStart();
            }

            void
            run() & noexcept
            {
                thread_ =
                    std::jthread([this](std::stop_token _stop_token) { do_rpc(_stop_token); });
            }

            void
            stop() & noexcept
            {
                thread_.request_stop();
                if (thread_.joinable()) { thread_.join(); }
            }

            Service&
            service() & noexcept
            {
                return service_;
            }

            grpc::Server&
            server() & noexcept
            {
                return *server_;
            }

            grpc::ServerCompletionQueue&
            completion_queue() & noexcept
            {
                return *cq_;
            }

            struct await_proxy {

                    std::coroutine_handle<>
                    await_suspend(std::coroutine_handle<> _awaiter) noexcept
                    {
                        void* empty = nullptr;
                        bool  s     = self_->writer_.compare_exchange_strong(
                            empty,
                            reinterpret_cast<void*>(
                                reinterpret_cast<std::uintptr_t>(_awaiter.address()) | kLockFlag),
                            std::memory_order_acquire,
                            std::memory_order_acquire);

                        if (!s) { return _awaiter; }
                        else
                        {

                            return std::noop_coroutine();
                        }
                    }

                    bool
                    await_ready() const noexcept
                    {
                        return self_->reader_ || self_->writer_.load(std::memory_order_relaxed);
                    }

                    request*
                    await_resume() const noexcept
                    {
                        if (!self_->reader_)
                        {
                            auto writes =
                                self_->writer_.exchange(nullptr, std::memory_order_acquire);

                            if (writes)
                            {
                                /* Reverse the linked list */
                                request* next = reinterpret_cast<request*>(writes);
                                request* temp = nullptr;
                                do
                                {
                                    temp           = next->next_;
                                    next->next_    = self_->reader_;
                                    self_->reader_ = next;
                                    next           = temp;

                                } while (next);
                            }
                        }

                        auto tmp       = self_->reader_;
                        self_->reader_ = tmp->next_;
                        return tmp;
                    };

                    grpc_service* self_;
            };

            await_proxy operator co_await() noexcept { return await_proxy{this}; }

        private:

            friend struct await_proxy;

            void
            do_rpc(std::stop_token _stop_token)
            {
                std::stop_callback callback(_stop_token, [this] { clean(); });

                while (!_stop_token.stop_requested())
                {
                    void* tag;   // uniquely identifies a request.
                    bool  ok;
                    while (true)
                    {
                        // Block waiting to read the next event from the completion queue. The
                        // event is uniquely identified by its tag, which in this case is the
                        // memory address of a CallData instance.
                        // The return value of Next should always be checked. This return value
                        // tells us whether there is any kind of event or cq_ is shutting down.
                        GPR_ASSERT(cq_->Next(&tag, &ok));

                        if (ok) { queue((request*) tag); }
                        else
                        {

                            ((request*) tag)->error();
                        }
                    }
                }
            }

            void
            queue(request* _item)
            {
                if (!_item)
                {
                    return;
                }

                auto current = writer_.load(std::memory_order_relaxed);
                do
                {
                    if (current)
                    {
                        const auto address = reinterpret_cast<std::uintptr_t>(current);

                        if (address & kLockFlag)
                        {

                            _item->next_ = nullptr;
                            writer_.store(_item, std::memory_order_release);

                            executer_.execute(reinterpret_cast<void*>(address & ~kLockFlag));

                            return;
                        }
                        else
                        {
                            _item->next_ = static_cast<request*>(current);
                        }
                    }
                    else
                    {
                        _item->next_ = nullptr;
                    }

                } while (!writer_.compare_exchange_weak(
                    current,
                    _item,
                    std::memory_order_release,
                    std::memory_order_relaxed));
            }

            void
            clean()
            {
                server_->Shutdown();
                cq_->Shutdown();
            }

            Executer executer_;

            std::jthread thread_;

            static constexpr std::uintptr_t kLockFlag = 0b1;

            std::atomic<void*> writer_;
            request*           reader_;

            std::unique_ptr<grpc::ServerCompletionQueue> cq_;

            Service service_;

            std::unique_ptr<grpc::Server> server_;
    };
}   // namespace co_grpc

#endif /* CO_GRCP_HPP_ */
