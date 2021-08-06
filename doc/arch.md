# HTTPNG architecture

(see doc/api.md for API documentation)

## Concept

httpng is the Tarantool module that implements HTTP(S) server
with direct access to the Tarantool process address space.
One or more threads (which we call "http threads") are created
to listen for TCP connections
(Unix sockets, UDP for HTTP/3 would probably be added later),
parse TLS and HTTP requests, "ask" Tarantool TX thread to process them
by user-specified "handler" written in Lua
(combined C/Lua handlers are planned for later),
then "postprocess" responses in the corresponding http thread
(status, headers, including ones to implement chunked encoding, body,
TLS, actual sending via network).

The idea here is to minimize Tarantool TX thread load - many operations,
including CPU-intensive ones like TLS encryption/decryption,
are performed in http threads.

## Details

libh2o from H2O HTTP server project (https://github.com/h2o/) is used
to parse HTTP protocol and handle TLS/transfers/buffering.
We may rewrite parts of libh2o or use our own code for transfers/buffering
later because as of 2021/08/06 libh2o code seems to be susceptible even
to easiest forms of (D)DoS - it can consume unbounded amounts of memory etc.

http threads of httpng listen for incoming connections,
accept them and feed to libh2o, which handles TLS and
details of the HTTP protocol, on success calling our callback with a structure
`h2o_req_t` representing parsed request.
This callback is not required to send a response immediately but it has to save
`h2o_req_t` content and everything it references - after return from this
callback libh2o can deallocate everything if HTTP(S) client would close
connection to the server. We use the name `shuttle` for a structure used
to pass data between http thread and TX thread - it is considered "owned"
by one of these threads and "thrown" back and forth
(although some fields can be accessed not by the owner,
namely request cancellation on HTTP(S) client connection closing).
At the moment there is only one way for libh2o to notify us that
HTTP(S) client has closed connection - by allocating "shared memory"
associated with this `h2o_req_t`, it can have a deallocation callback.
(TODO: Modify libh2o to add similar callback and extra data to `h2o_req_t`,
this is just performance optimization).
So we allocate a small `anchor` structure that contains a pointer to `shuttle`
and the callback performs necessary clean up,
notifying TX thread to cancel request processing.
All "postprocess" functions, launched in http threads,
check that `shuttle` has not yet been disposed and do not dereference
`h2o_req_t` pointer in that case (and, of course, send nothing).
tarantool/xtm is used to call functions in the TX thread from a http thread
and vice versa. Two xtm queues are created for each http thread -
one in each direction. Separate fiber is created to handle requests from
each http thread.
(TODO: Check that using single fiber to handle all xtm queues -
which are single-producer-single-consumer - is doable with reasonable efforts
and actually works faster, this is just performance optimization).
Request processing code in TX thread creates new fiber for every request,
creates Lua tables with request data, and launches user-configured handler.
Such a handler can send a response and/or terminate request handling ("close").

## Response sending design

### Currently
`write_header()`/`write()`/`close()` blocks until
send has completed, this way we send payload directly from Lua strings
and do not even need to reference them. This approach, however,
has throughput and latency issues - user handler can't send anything else
or even do something useful (unless it explicitly creates another fiber
to do the heavy lifting) until `write()` etc return.
This can be solved by implementing send buffering in TX thread.
There is, however, another approach.

### Proposed design, stage 1:
`write_header()`/`write()` references Lua string(s) to send
and returns immediately - unless `shuttle` is owned by http thread
(by previous `write_header()`/`write()`).

### Proposed design, stage 2:
`close()` returns immediately unless `shuttle` is owned by http thread.

### Proposed design, stage 3:
`close()` never blocks; if `shuttle` is owned by http thread,
it sets a flag in `shuttle` meaning "...and then terminate request handling".
If http thread would see that flag in time
(this is easily implemented with atomic cmpxchg), everything is perfect
(fewer calls to libh2o functions, fewer network packets in some cases).
If not, another request to http thread would be created on `shuttle`
return to TX thread ownership.

### Proposed design, stage 4:
`write()` attempts to never block - builds a chain of buffers to send
(and references to release on send completion)
and notifies http thread about chain modifications (atomic cmpxchg etc).
Subject to various limitations so it may still block on excessive sends -
there is limited space in `shuttle`,
maximal number of queued buffers and payload bytes should be limited
to avoid consuming too much Lua memory and starving other handlers.

## Known issues

libh2o passes whole http `body` with `h2o_req_t`.
This is not scalable and prone to DoS - `body` can easily be gigabytes or more
while `shuttle` is designed to be a few kilobytes in size.
libh2o 2.2.6 does not have body streaming and allocates an unlimited buffer
to hold [decrypted] data received via a network
(this is where `h2o_req_t->entity.base` points).
libh2o, however, has an ability to configure maximal allowed `body` size
which we use implicitly (and allow a user to set stricter limit).
TODO: Upgrade to the latest libh2o and/or implement `body` streaming there,
support it in httpng.

libh2o 2.2.6 does not survive `malloc()` returning NULL - there is no error handling
(like sending HTTP status 500 and/or deallocating resources), just `abort()`.
This should be fixed for httpng to be production-ready.

## Preventing (D)DoS

- Maximal number of connections is configurable.
- Backlog size (for `listen()`) is configurable.
- Maximal number of `shuttle`s is configurable
(this also limits the maximal number of fibers created).
TODO: Stop accepting new connections and stop reading from all sockets
(maybe just HTTP/2+ ones?) when approaching such a limit,
this may be more desireable than returning HTTP status 500
to HTTP(S) client - but then all clients would suffer
increased delays. This should be configurable.
- xtm queue size is configurable.
- maximal HTTP `body` size is configurable.
