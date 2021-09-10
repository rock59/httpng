local t = require('luatest')
local http = require 'httpng'
local fiber = require 'fiber'
local ssl_pairs = require 'tests.ssl_pairs'
local popen
pcall(function() popen = require 'popen' end)
local _

local listen_with_single_ssl_pair = {
    tls = {
        ssl_pairs['foo'],
    }
}

local using_popen = function()
    return popen ~= nil
end

local real_handler
local function universal_handler(req, io)
    return real_handler(req, io)
end

local ensure_can_start_and_kill_processes = function()
    --t.skip('simulating broken process launch or kill')
end

local httpng_configured = false
local my_http_cfg = function(cfg)
    if httpng_configured then
        return
    end
    http.cfg(cfg)
    httpng_configured = true
    if not using_popen() then
        -- FIXME: Wait until initialized? To investigate.
        fiber.sleep(0.1)
    end
end

local my_shell_internal = function(cmd, stdout)
    if not using_popen() then
        os.remove 'tmp_pid.txt'
        if os.execute('tests/process_helper ' .. cmd) ~= 0 then
            return nil
        end
    ::retry_pid::
        local file = t.assert(io.open 'tmp_pid.txt')
        if (file == nil) then
            fiber.sleep(0.001)
            goto retry_pid
        end
        local pid = file:read '*a'
        file:close()
        return pid
    end

    local opts = {}
    opts.shell = true
    opts.setsid = true
    opts.groupsignal = true
    opts.stdout = stdout
    opts.close_fds = false -- That's the point
    return popen.new({cmd}, opts)
end

local my_shell_r = function(cmd)
    return my_shell_internal(cmd, popen.opts.PIPE)
end

local bench_handler_hello = function(req, io)
    return {
        status = 200,
        body = 'Hello, World!',
    }
end

local s

local bench_handler_query = function(req, io)
    local payload
    local req_query = req.query
    if req_query then
        local query_str = string.match(req_query, '^id=%d+')
        if query_str then
            local id_str = string.sub(query_str, 4, -1)
            local id = tonumber(id_str)
            if id then
                local tuple = s:get(id)
                if tuple then
                    payload = tuple.desc
                else
                    payload = 'Entry was not found'
                end
            else
                payload = 'Invalid id was specified (not a number)'
            end
        else
            payload = 'Unable to parse query (format: "?id=3")'
        end
    else
        payload = 'No query specified'
    end

    return {
        status = 200,
        body = payload,
    }
end

local bench_space_created = false
local create_bench_space = function()
    if bench_space_created then
        return
    end
    box.cfg{
        listen = 3306,
        wal_mode = 'none',
        checkpoint_interval = 0,
    }

    s = box.schema.space.create 'tester'
    s:format{
        {name = 'id', type = 'unsigned'},
        {name = 'desc', type = 'string'},
    }
    local index = s:create_index('primary', {
        type = 'tree',
        parts = {'id'}
    })

    s:insert{1, 'First'}
    s:insert{2, 'Second'}
    bench_space_created = true
end

g_benchmarks = t.group 'benchmarks'

local test_bench = function(handler, options, use_tls)
    t.skip_if(not using_popen(), "can't use redirected I/O")
    create_bench_space()

    real_handler = handler
    local cfg = { handler = universal_handler, threads = 3 }
    local proto
    if (use_tls) then
        cfg.listen = listen_with_single_ssl_pair
        proto = 'https'
    else
        proto = 'http'
    end
    my_http_cfg(cfg)

    ensure_can_start_and_kill_processes()
    if proto == 'http' then
        t.skip('insecure HTTP is not yet supported')
    end
    local target
    if using_popen() then
        target = ' '
    else
        target = ' -o tmp_bench.txt '
    end
    local cmd = 'h2load -D 60 -c 22 -t 2 ' .. options .. ' ' ..
        proto .. '://localhost:3300/?id=2'
    local output
    local ph = my_shell_r(cmd)
    local result = ph:wait().exit_code
    output = ph:read()
    t.assert_equals(result, 0, 'h2load failed')
    local total, failed
    for s in output:gmatch "[^\r\n]+" do
        total, failed = string.match(s,
            'requests: ([0-9]+) total, [0-9]+ started, [0-9]+ done,' ..
            ' [0-9]+ succeeded, ([0-9]+) failed, [0-9]+ errored,' ..
            ' [0-9]+ timeout.*')
        if total ~= nil then
            goto found
        end
    end
    t.fail 'no valid statistics from h2load found'

::found::
    t.assert(failed ~= nil, 'statistics format invalid')
    total = tonumber(total)
    failed = tonumber(failed)
    t.assert(total > 0 and failed == 0)
end

g_benchmarks.test_bench_query_http1_tls = function()
    test_bench(bench_handler_query, '--h1', true)
end

g_benchmarks.test_bench_query_http2_tls = function()
    test_bench(bench_handler_query, '', true)
end

g_benchmarks.test_bench_hello_http1_tls = function()
    test_bench(bench_handler_hello, '--h1', true)
end

g_benchmarks.test_bench_hello_http2_tls = function()
    test_bench(bench_handler_hello, '', true)
end
