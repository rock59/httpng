box.cfg{
    listen = 3306,
    wal_mode = 'none',
    checkpoint_interval = 0,
}

local s = box.schema.space.create('tester')
s:format({
    {name = 'id', type = 'unsigned'},
    {name = 'desc', type = 'string'},
})
local index = s:create_index('primary', {
    type = 'tree',
    parts = {'id'}
})

s:insert{1, 'First'}
s:insert{2, 'Second'}

local http = require 'httpng'
local fiber = require 'fiber'

local touch_db = function()
    local tuple = s:get(2)
end

local foo_handler = function(req, io)
    touch_db()
    return {
        status = 200,
        body = 'foo',
    }
end

local alt_foo_handler = function(req, io)
    touch_db()
    return {
        status = 200,
        body = 'FOO',
    }
end

local bar_handler = function(req, io)
    touch_db()
    return {
        status = 200,
        body = 'bar',
    }
end

local alt_bar_handler = function(req, io)
    touch_db()
    return {
        status = 200,
        body = 'BAR',
    }
end

local config = {
    threads = 4,
    listen = {
        port = 8080,
        tls = { require 'examples.ssl_pairs'.foo },
    },
}

::again::

local router_module = http.router
print 'Using foo..'

local router = router_module.new()
router:route({path = '/subdir'}, alt_foo_handler)
router:route({path = '/'}, foo_handler)
config.handler = router
http.cfg(config)
fiber.sleep(0.1)

print 'Using bar..'
local router = router_module.new()
router:route({path = '/subdir'}, alt_bar_handler)
router:route({path = '/'}, bar_handler)
config.handler = router
config.listen = nil
http.cfg(config)

fiber.sleep(0.1)
goto again
