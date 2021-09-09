# HTTPNG Lua API (proposed changes for review)

## Idea

Some HTTP(S) clients may receive data from the HTTP(S) server much slower than
expected by the Lua handler which sends it.
This may be caused by low-performance client hardware or, more likely,
a slow or overloaded network connection between
the server and the client. This may not matter for tasks like downloading
a large file but is very important for certain near-real-time applications,
e. g. sending current stock market data -
it is considered obsolete in a few seconds or even less.
Such tasks do not need buffering which delays data for several
minutes, it is preferred to drop some data instead (and maybe notify the client
that data loss has occurred).

To do that, the Lua handler should know that sends are delayed
and what has already been sent via a network. This assumes non-blocking sends
from the Lua handler are implemented (at the moment sends are blocking).

## Gory details

(only changes from doc/arch.md are here)

...
### cfg table

- `default_send_window`: Integer, specifies max number of `body` bytes
in flight from the handler to TCP/IP stack until `write*()` functions
would block. Please note that it is not related to TCP window size.
Defaults to 1048576.

- `default_send_timeout`: Number, specifies a timeout in seconds for
`write*()` call to pass data to TCP/IP stack. Defaults to 30.

...

### Handlers

...

`function handler(req, io)`

...

- `io`: Table with the following entries:
...
  - `write_header(io, code, headers, timeout)`: function,
...
Returns `nil` or `send_result` (see below).
Note that you **MUST** check return value if it is not `nil` and timeout
is not infinity because some data may not be sent in that case.
...

  - `write(io, body, timeout)`: function,
...
Returns `nil` or `send_result` (see below).
Note that you **MUST** check return value if it is not `nil` and timeout
is not infinity because some data may not be sent in that case.
...

  - `send_window`: Integer, specifies the max number of `body` bytes
in flight from HTTP(S) server to HTTP(S) client until `write*()` functions
would block. Please note that it is not related to TCP window size.
Can be nil, `default_send_window` is used in that case.

- `send_timeout`: Number, specifies a timeout in seconds for
`write*()` call to pass data to TCP/IP stack.
Used only if positional `timeout` parameter to `write*()` is not specified.
Can be nil, `default_send_timeout` is used in that case.

  - `get_send_stats(io)`: function, returns `send_stats` table
(see below).

  - `wait(io, pos, timeout)`: function, yield
until `send_stats.sent >= pos` (if `pos > 0`)
or `send_stats.current - send_stats.sent >= -pos` (if `pos <= 0`)
or `send_stats.cancelled == True` or `timeout` seconds passed.
Returns `send_result`.

#### Send result

`write*()`/`wait()` functions can return an opaque value
which we call `send_result` (for performance reasons it could be an Integer
but you should not make any assumptions about that).
You can use helper functions from `require 'httpng'` table
to decode `send_result`.
- `cancelled(send_result)`: Returns Boolean,
`True` if HTTP(S) client has already cancelled
request processing by closing/resetting TCP connection (or just timing out)
or in any other way. All further `write*()` to this request's `io` are silently
ignored. User handler can use this information to avoid wasting server
resources - it is recommended to free resources (if applicable) and return.
Call to this function with `nil` would return `nil`.
- `headers_were_sent(send_result)`: Returns Boolean, `True` if `code`/`headers`
specified in a call to `write_header()` were passed to TCP/IP stack.
It is unspecified
what this function would return when called with `send_result` from `write()`
or `wait()`, it is allowed (but *not* guaranteed) to `error()` in such a case.
Call to this function with `nil` would return `True`.
- `body_bytes_sent(send_result)`: Returns Integer - how many bytes of `body`
has been passed to TCP/IP stack. If it is less than the size of `body` specified,
remaining bytes **ARE NOT SENT**. It is unspecified what this function
would return when called with `send_result` from `wait()`, it is allowed
(but **not** guaranteed) to `error()` in such a case.
Call to this function with `nil` would `error()`.

#### Send stats

`get_send_stats()` returns table which we call `send_stats`.
It contains the following members:
 - `current`: Integer, number of `body` bytes passed to `write()`.
 - `sent`: Integer, number of `body` bytes passed to TCP/IP stack.
(note that some of them may still be "in flight" via network).
 - `cancelled`: Boolean, see `cancelled(send_result)` for a description.
 - `headers_were_sent`: Boolean, `True` if `code`/`headers`
were passed to TCP/IP stack (they may still be "in flight" via network).

Example:
```
io:send_timeout = 500 * 365 * 24 * 3600 -- Approx. 500 years

--- send_stats: current = 0, sent = 0, headers_were_sent = False

-- Sends status, headers
io:write_header(200, {['content-type'] = 'text/plain'})

--- send_stats: current = 0, sent = 0, headers_were_sent = True

-- Sends 3 bytes of body
io:write('abc')

--- send_stats: current = 3, sent = 3, headers_were_sent = True

io:write('12345') -- Sends another 5 bytes of body but the call is blocking
                  -- or data is not yet delivered to HTTP thread or buffered

--- send_stats: current = 8, sent = 3, headers_were_sent = True

--- Some time has passed, data has been sent to the HTTP(S) client

--- send_stats: current = 8, sent = 8, headers_were_sent = True

```
Please note that it is not guaranteed that `sent` is updated
in the same chunks you send (it can be adjusted byte-by-byte
or in any other step, including everything sent at once).

You can implement any timeouts needed for your application by recording
timestamps and `send_stats.current` before calling `write()` and comparing it
to `send_stats.sent` whenever appropriate.
You can use `wait()` to efficiently wait for events.
