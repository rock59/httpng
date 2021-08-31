# HTTPNG Lua API
(we would probably also implement some kind of C API in the future)

## Quick start

``` lua
local httpng = require 'httpng'
local fiber = require 'fiber' -- Only to use fiber.sleep() in this example

local hello = function(req, io)
    return { body = 'Hello, World!' }
end

local cfg = { handler = hello }

-- Configure and start HTTP(S) server.
httpng.cfg(cfg)

-- Do whatever you please, HTTP(S) server is serving requests now.
fiber.sleep(60)
```

## Gory details

httpng module exports the following:

- `cfg()`: configure and start HTTP(S) server; reconfigure HTTP(S) server launched earlier (hot reload). Accepts and requires a single parameter - table with server configuration, described below (with hot reload details).

### cfg table

- `handler`: HTTP(S) request handler - a function (which handles all requests)

- `max_conn_per_thread`: Integer, max number of HTTP(S) TCP connections per thread.

- `max_shuttles_per_thread`: Integer, max number of simultaneous
HTTP(s) requests processed by one thread (see `shuttle_size`).

- `min_proto_version`: String, sets minimal accepted SSL/TLS protocol. Defaults to 'tls1.2'. Accepted values are 'ssl3', 'tls1', 'tls1.0', 'tls1.1', 'tls1.2', 'tls1.3'.

- `openssl_security_level`: Integer, defaults to 1. Please see OpenSSL 1.1* documentation for details.

- `shuttle_size`: Integer, specifies the max size in bytes of
the internal buffer used to pass data between HTTP(S) server threads
and TX thread. Defaults to 65536.
One "shuttle" is used for every HTTP(S) request.
Lowering its value helps decrease memory usage but limits maximum HTTP(S)
request (not HTTP(S) response!) body size
as well as the maximal accepted size of request headers.

- `threads`: Integer, how many threads to use for HTTP(S) requests processing. It is unlikely that you would need more than 4 because performance is limited by Lua processing which is performed in the TX thread even if you do not access the database. Defaults to 1.

### Handlers

`function handler(req, io)`

This is what HTTPNG is about - handling HTTP(S) requests. Handlers are Lua functions that run in separate fibers in the TX thread (and can access the Tarantool database if they want to).

- `req`: Table with the following entries:
  - `body`: String, HTTP(S) request body.
  - `headers`: Table containing HTTP(S) request headers with entries like `['user-agent'] = 'godzilla'`
  - `method`: String, 'GET', 'PUT' etc.
  - `path`: String, contains "path" of HTTP(S) request - that is, '/en/download' for 'https://www.tarantool.io/en/download?a=b'.
  - `query`: String, everything after "?" in path or `nil`.
  - `_shuttle`: Userdata, please do not touch.

- `io`: Table with the following entries:
  - `close(io)`: function,
finishes HTTP(S) request handling.
You do not need that in most cases because return from handler always does that.
`io` is a reference to self - `io:close()`.
  - `headers`: Empty table where you can create entries containing HTTP(S) response headers like `['content-type'] = 'text/html'`. It is used if you do not specify `headers` when calling `write_header()` or `write()`.
  - `write_header(io, code, headers)`: function,
sends HTTP(S) `code` (Integer),
`headers` (optional Table with entries like
`['content-type'] = 'text/plain; charset=utf-8'`;
if it is not specified then `io.headers` is used).
Returns `True` if the connection has already been closed so there is no point
in trying to send anything else. `io` is a reference to self -
`io:write_header(code, headers)`.
`write_header()` can be called only once per HTTP(S) request.
Note that libh2o may add some headers to handle chunked encoding etc.
  - `write(io, body)`: function, sends `body` (String).
Returns `True` if the connection has already been closed
so there is no point in trying to send anything else.
`io` is a reference to self - `io:write(payload)`.
If there was no call to `write_header()` earlier, HTTP(S) code would be 200
and `io.headers` would be used. Note that libh2o may add some headers
to handle chunked encoding etc.
  - `_shuttle`: Userdata, please do not touch.

`handler()` can optionally return a table with `status`, `headers` and `body`,
the effect is the same as `io:write_header(status, headers); io:write(body)`
(if `io:write_header()` was not called; note that `io.headers` is *not* used)
or `io:write(body)` (if `io:write_header()` was called earlier;
`status` and `headers` are silently ignored).
