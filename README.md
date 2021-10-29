# co_grpc

An coroutine wrapper for gRPC. This repository provides a header only library for converting the gerneral format of running an asynchronous gRPC service (as show in the [example](https://grpc.io/docs/languages/cpp/async/)) to a `co_await`'able thread safe object. 

This implementation differs to things like `[asio-grpc](https://github.com/Tradias/asio-grpc)` as this library is coroutine/executor agnostic. Instead of modeling each request as a coroutine or fibers of execution, co_grpc converts the gerneral architecture of async grpc into an co_await'able type. All you need to do is co_await incoming requests then you can handle them in any way you want.

## Usage 

Just include the `co_grpc.hpp` header into your project. This header is not self-contained as it requires the `#include <grpcpp/grpcpp.h>` to be included before `co_grpc.hpp`. This is not ideal, but the other option is having the entire grpc library as a sub-module which then makes it difficult as `co_grpc` is more or less grpc version agnostic. (A fix for this is inbound.)

co_grpc requires coroutines and as such will require a c++ compiler with c++20 and coroutine support. So far it has only been tested on g++-10+.

## Asynchronous Service

It is easier to use the `grpc_service` as a name alias in your project. Say you have a `Server` proto service definition (see [example](#Example)). The you can alias you service as:
```c++
/* More on what executor should be below. */
using example_service = grpc_service<example::ExampleServer::AsyncService, Executor>;
```

Then setting up your service in a similar way to the [async example](https://grpc.io/docs/languages/cpp/async/). A bit of the boiler plate is removed:
```c++
example_service service;
service.build("<ip|hostname>:<port>", <some grpc credentialing system>);
service.run();

std::cout << "Service is running!" << "\n";
# now your service is running
```c++
For example "<ip|hostname>:<port>" can be "localhost:5051". <some grpc credentialing system> can be what ever security mode you want to run in (unsecured, ssl, ...). For example either `grpc::InsecureServerCredentials` or `grpc::SslServerCredentials`.

To process incoming messages you can run the simple loop:
```c++
while (true)
{
    example_service::request* req = co_await service;

    req->proceed();
}
```

`co_await service;` server is not thread safe.

See [Message Inheritance](#Message-Inheritance) for more details about `example_service::request`.

## Coroutine Executor
`co_await service` will not suspend if there is a request waiting, but will if there is not. In the case that `co_await service;` suspends, co_grpc needs a way to resume the suspended coroutine and hopefully leaving the co_grpc context. The user must provide an `Executor` to do so. In this library an `Executor` is simply some object callable with `void*`. The `void*` is the memory region of the coroutine where the coroutine handle can be accessed through `coroutine_handle<>::from_address()`.

The Executor will be called in the in the co_grpc thread. The Executor should ideally resume the coroutine in a different context. You could process the request here, but it would diminish the asynchronous nature of the service, as co_grpc wont be able to process other requests in the background. An example executor using the [ZAB coroutine framework](https://github.com/Donald-Rupin/zab) is:
```c++
struct grpc_executor {

    grpc_executor(zab::engine* _e) : e_(_e) { }

    void
    execute(void* _object)
    {
        e_->resume(
            std::coroutine_handle<>::from_address(_object),
            zab::order::now(),
            zab::thread::in(0 /* some thread you want move into */));
    }

   zab::engine* e_;
};
```

## Message Inheritance.

The library is designed to have one class per `rpc` call. These classes need to inherit from `example_service::request` (a nested class type). Again the usage is pretty similar to that shown in `[grpc example](https://grpc.io/docs/languages/cpp/async/`. 

`request` provides the following interface:

/*
 * Do the next thing required. This will either be process some application logic,
 * all clean up this object. Calling this function may invalidate the object. 
 * 
 */
void
proceed();

/*
 * Mark this request as finished. Next `proceed();` will clean it up.
 * 
 */
inline void
complete() noexcept;

/*
 * Get the underlying co_grpc service type.
 * 
 */
inline grpc_service<S, E>&
server() noexcept;

/*
 * Get the grpc server's context. Used for registering and reply to messages.
 * 
 */
inline grpc::ServerContext&
context() noexcept;

`request` has the following pure virtual functions:

```c++
/*
 * If `complete()` has not bee called. Calling `proceed();` will trigger this function.
 * 
 * This should be the main body of the request that does your application logic.
 *
 * You application should call `complete()` when it is marking itself for cleanup on next `proceed()`.
 * Generally this should be when you call the equivalent grpc `Finish(...)` method.
 *
 */
virtual void
process() = 0;

/*
 * Calling `proceed();` for the first time will trigger this function.
 * 
 * You class should create a new instance of itself and register it for getting the next request.
 *
 */
virtual void
clone() = 0;
```

`request` also has the following virtual functions:

```c++
/*
 * This is called in the co_grpc context if something to do with the request failed.
 *
 * The most common example is if a client disconnect a stream before it is finished. 
 * This class will not receive any more requests and should be destroyed.
 *
 * If deconstruction of this class is required to be thread safe (not running in the grpc context),
 * a custom `error()` function should be provided.
 *
 */
virtual void
error(){
	destroy();
};

/*
 * If `complete()` has been called. Calling `proceed();` will trigger this function.
 * 
 * The default assumes that `clone()` and the original allocation was made using `new`.
 * If this is not the case, then a custom `destroy()` should be provided.
 *
 */
virtual void
destroy()
{
    delete this;
}
```

## Example
Say we have the proto definitions:

```proto

package example;

message Hello {
	std::string greeting = 1;	
}

message Goodbye {
	std::string farewell = 1;	
}

service ExampleServer {
	
 	rpc SayHello (Hello) returns (Goodbye) {}

}

```

We have the badly implemented Executor (not going to leave the grpc context). 
```c++
struct bad_executor {

    void
    execute(void* _object)
    {
        std::coroutine_handle<>::from_address(_object)
    }
};


Our service alias.
```c++
using example_service = grpc_service<example::ExampleServer::AsyncService, bad_executor>;
```

The request.
```c++
class SayHello : public example_service::request {
	public:

		using super = example_service::request;

		SayHello(example_service& _service)
			: super(_service), responder_(&context())
        {
            /* Register for the next request */
            server().service().RequestSayHello(
                    &context(),
                    &request_,
                    &responder_,
                    &server().completion_queue(),
                    &server().completion_queue(),
                    (void*) this);
		}

        void
        process() override
        {
            std::cout << *request_.mutable_gretting() << "\n";

            reply_.set_farewell("Bye!");

            /* We are done! */
            complete();
            responder_.Finish(reply_, grpc::Status::OK, this);
        }

        void
        clone() override
        {
            new SayHello(server());
        }

        // use default error and destroy. 

	private:

        grpc::ServerAsyncResponseWriter<example::Goodbye> responder_;

        example::Hello request_;

        example::Goodbye reply_;
};

```

Now run:
```c++
some_coroutine_type 
run_server()
{
	example_service service;
	service.build("localhost:50051", grpc::InsecureServerCredentials());
	service.run();

    new SayHello(service);

	while (true)
	{
    	auto* req = co_await service;
    	/* Since we use bad_executor here will be executing in the grpc thread */
    	req->proceed();
	}
}
```
