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
in flight from HTTP(S) server to HTTP(S) client until `write*()` functions
would block. Please note that it is not related to TCP window size.
Defaults to 1048576.

- `default_send_timeout`: Number, specifies a timeout in seconds for blocking
`write*()` call to wait for a response from HTTP(S) server thread.
Defaults to 1.

...

### Handlers

...

`function handler(req, io)`

...

- `io`: Table with the following entries:
...
  - `write_header(io, code, headers)`: function,
...
Returns `nil` or `send_result` (see below).
...
  - `write_header_nb(io, code, headers)`: function,
equivalent to `write_header()` but it never blocks
(see `send_result` for details).

  - `write(io, body)`: function,
...
Returns `nil` or `send_result` (see below).
...

  - `write_nb(io, body)`: function,
equivalent to `write()` but it never blocks
(see `send_result` for details).

  - `send_window`: Integer, specifies the max number of `body` bytes
in flight from HTTP(S) server to HTTP(S) client until `write*()` functions
would block. Please note that it is not related to TCP window size.
Defaults to `default_send_window`.

  - `send_timeout`: Number, specifies a timeout in seconds for blocking
`write*()` call to wait for a response from HTTP(S) server thread.
Defaults to `default_send_timeout`.

  - `get_send_stats(io)`: function, returns `send_stats` table
(see below).

  - `block_until_remains(io, leftover, timeout)`: function, yield
until `send_stats.current - send_stats.sent >= leftover`
or `send_stats.cancelled == True` or `timeout` seconds passed.
Returns `send_result`.

  - `block_until_pos(io, pos, timeout)`: function, yield
until `send_stats.sent >= pos` or `send_stats.cancelled == True`
or `timeout` seconds passed.
Returns `send_result`.

#### Send result

`write*()`/`block*()` functions can return an opaque value
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
- `timed_out(send_result)`: Returns Boolean,
set to `True` if at least part of the `body`
specified in this particular call to `write*()`
has not been sent to the HTTP(S) client for `send_timeout` seconds.
This part of the `body` will still be delivered to HTTP(S) client later
(unless the request is cancelled [later]) - but please read a comment below.
It is guaranteed to not be `True` if `cancelled(send_result)`
to make handlers faster and simpler.
`_nb` versions of functions never block so they can't time out.
- `would_block(send_result)`: Returns Boolean,
only `_nb` versions of functions can use that.
`True` if an attempt to send data would block the caller's fiber
(e. g. number of `body` bytes in flight plus length of `body` in this
`write*()` call is larger than `send_window`)
or would cause `body` data to be queued in TX
because an earlier call has timed out.
Nothing is sent to HTTP(S) client in this case. The caller of `_nb` versions
of functions is expected to always handle `would_block(send_result) == True`.
It is guaranteed to not be `True` if `cancelled(send_result)`
to make handlers faster and simpler.

Please note that `timed_out` is only set if this particular call to one of
`write*()`/`block*()` functions was blocking and has timed out.
`timed_out == False` does NOT mean that data has been sent to
the HTTP(S) client.
You can use `get_send_stats()` to determine that
and measure timing according to your application needs.

#### Send stats

`get_send_stats()` returns table which we call `send_stats`.
It contains the following members (note that indexes are 1-based):
 - `current`: Integer, number of `body` bytes passed to `write*()` functions.
 - `sent`: Integer, number of `body` bytes sent to the HTTP(S) client
(note that some of them may still be "in flight" via network).
 - `cancelled`: Boolean, see `cancelled(send_result)` for a description.

Example:
```
--- send_stats: current = 0, sent = 0

-- Sends status, headers and 3 bytes of body
io:write_header(200, {['content-type'] = 'text/plain'}, 'abc')

--- send_stats: current = 3, sent = 3

io:write('12345') -- Sends another 5 bytes of body but the call is blocking
                  -- or data is not yet delivered to HTTP thread or buffered

--- send_stats: current = 8, sent = 3

--- Some time has passed, data has been sent to the HTTP(S) client

--- send_stats: current = 8, sent = 8

```
Please note that it is not guaranteed that `sent` is updated
in the same chunks you send (it can be adjusted byte-by-byte
or in any other step, including everything sent at once).

You can implement any timeouts needed for your application by recording
timestamps and `send_stats.current` before calling `write*()` and comparing it
to `send_stats.sent` whenever appropriate.
You can use `block_*()` functions to efficiently wait for events.
