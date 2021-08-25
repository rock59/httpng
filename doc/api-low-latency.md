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
  - `write_header(io, code, headers, body)`: function,
...
Returns `nil` or `send_result` (see below).
...
  - `write_header_nb(io, code, headers, body)`: function,
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

  - `block_until_acknowledged(io, index, timeout)`: function, yield
until `send_stats.acknowledged >= index`
or `send_stats.cancelled` or `timeout` seconds passed.

  - `block_until_everything_acknowledged(io, timeout)`: function, yield
until `send_stats.acknowledged == send_stats.current`
or `send_stats.cancelled` or `timeout` seconds passed.

  - `block_until_everything_sent(io, timeout)`: function, yield
until `send_stats.sent == send_stats.current` or `send_stats.cancelled == True`
or `timeout` seconds passed.
Please note that when this function returns there may still be part of `body`
"in flight" from HTTP(S) server to HTTP(S) client.

  - `block_until_sent(io, index, timeout)`: function, yield
until `send_stats.sent >= index` or `send_stats.cancelled == True`
or `timeout` seconds passed.
Please note that when this function returns there may still be part of `body`
"in flight" from HTTP(S) server to HTTP(S) client.

#### Send result

`write_header()` and `write()` can return table which we call `send_result`.
It can contain one or more of the following members:
- `cancelled`: Boolean, set to `True` if HTTP(S) client has already cancelled
request processing by closing/resetting TCP connection (or just timing out)
or in any other way. All further `write*()` to this request's `io` are silently
ignored. User handler can use this information to avoid wasting server
resources - it is recommended to free resources (if applicable) and return.
- `timed_out`: Boolean, set to `True` if at least part of the `body`
specified in this particular call to `write*()`
has not been acknowledged by the HTTP(S) client for `send_timeout` seconds.
This part of the `body` will still be delivered to HTTP(S) client later
(unless request is cancelled [later]).
- `would_block`: Boolean, applies only to `_nb` versions of functions.
Set to `True` if an attempt to send data would block the caller's fiber
(e. g. number of `body` bytes in flight plus length of `body` in this
`write*()` call is larger than `send_window`)
or would cause `body` data to be queued in TX
because an earlier call has timed out.
Nothing is sent to HTTP(S) client in this case. The caller of `_nb` versions
of functions is expected to always handle `would_block == True`.

Please note that `timed_out` is only set if this particular call to one of
`write*()` functions was blocking and has timed out.
`timed_out == False` does NOT mean that data has been acknowledged by
the HTTP(S) client.
You can use `get_send_stats()` to determine that
and measure timing according to your application needs.

#### Send stats

`get_send_stats()` returns table which we call `send_stats`.
It contains the following members (note that indexes are 1-based):
 - `current`: Integer, index of next `body` byte which
`write*()` functions would send.
 - `sent`: Integer, index of first `body` byte not yet sent
to the HTTP(S) client.
 - `acknowledged`: Integer, index of first `body` byte not yet acknowledged
by the HTTP(S) client.
 - `cancelled`: Boolean, see `send_result.cancelled`.

Example:
```
--- send_stats: current = 1, sent = 1, acknowledged = 1

-- Sends status, headers and 3 bytes of body
io:write_header(200, {['content-type'] = 'text/plain'}, 'abc')

--- send_stats: current = 4, sent = 4, acknowledged = 1

io:write('12345') -- Sends another 5 bytes of body

--- send_stats: current = 9, sent = 9, acknowledged = 1

fiber.sleep(0.01)
-- HTTP(S) client has received and acknowledged part of sent data.

--- send_stats: current = 9, sent = 9, acknowledged = 4

fiber.sleep(1)
-- HTTP(S) client has received and acknowledged everything.

--- send_stats: current = 9, sent = 9, acknowledged = 9
```
Please note that `sent` may be less than `current` if queueing happens in TX
(e. g. if you are calling `write*()` after `timed_out == True`).
Please note that transport protocols do not guarantee that data is acknowledged
in the same chunks you send (it is allowed to acknowledge bytes one-by-one
or in any other step, including everything sent at once).

You can implement any timeouts needed for your application by recording
timestamps and `send_stats.current` before calling `write*()` and comparing it
to `send_stats.acknowledged` whenever appropriate.
You can use `block_*()` functions to efficiently wait for events.
