local http = require 'httpng'
local fiber = require 'fiber'

http.cfg{
    threads = 4,
    handler = function(req, io)

        io.headers['x-req-id'] = 'abc';
        io:write_header(200);

        io:write("some data+")
        fiber.sleep(1)
        io:write("some data!")
        io:close()

    end
}
