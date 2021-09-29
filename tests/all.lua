local t = require('luatest')
local http = require 'httpng'
local fiber = require 'fiber'
local popen
pcall(function() popen = require 'popen' end)
local curl_bin = 'curl'
local router_module
local router_module_c
local router_checked = false
local debug_wait_process = require 'test_helpers'.debug_wait_process

local stubborn_handler = function(req, io)
::again::
    local closed = io:write('foobar')
    if closed then
        -- Connection has already been closed
        return
    end
    goto again
end

local stubborn2_handler = function(req, io)
    local start = fiber.clock()
::again::
    local closed = io:write('foobar')
    if closed then
        -- Connection has already been closed
        return
    end
    if (fiber.clock() - start >= 0.5) then
        return
    end
    goto again
end

local shutdown_support_checked = false
local shutdown_works = false

local test_shutdown = function()
    http.cfg{handler = function() end}
    local err
    shutdown_works, err = pcall(http.shutdown)
    shutdown_support_checked = true
    return err
end

local ensure_shutdown_works = function()
    if not shutdown_support_checked then
        test_shutdown()
        assert(shutdown_support_checked)
    end
    t.skip_if(not shutdown_works,
        'This test requires httpng.shutdown() to work')
end

local using_popen = function()
    return popen ~= nil
end

local ensure_can_start_and_kill_processes = function()
    --t.skip('simulating broken process launch or kill')
end

--[[
    Actually it now checks that we can launch processes and kill them,
    not necessarily using popen module (which is more efficient).
    Please use ensure_can_start_and_kill_processes() in new code.
--]]
local ensure_popen = function()
    --t.skip_if(popen == nil, 'This test requires popen')
    ensure_can_start_and_kill_processes()
end

local my_http_cfg = function(cfg)
    http.cfg(cfg)
    if not using_popen() then
        -- FIXME: Wait until initialized? To investigate.
        fiber.sleep(0.1)
    end
end

local get_client_result = function(ph)
    if ph == nil then
        return nil
    end

    local result
    if using_popen() then
        return ph:wait().exit_code
    end

    local ok, status = pcall(debug_wait_process, ph)
    if not ok then
        error('Unable to determine process exit code, reason: ' .. status)
    end
    return status
end

local my_shell_internal = function(cmd, stdout)
    if not using_popen() then
        os.remove 'tmp_pid.txt'
        if (os.execute('tests/process_helper ' .. cmd) ~= 0) then
            return nil
        end
    ::retry_pid::
        local file = assert(io.open 'tmp_pid.txt')
        if (file == nil) then
            fiber.sleep(0.001)
            goto retry_pid
        end
        local pid = file:read('*a')
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

local my_shell = function(cmd)
    return my_shell_internal(cmd)
end

local my_shell_r = function(cmd)
    return my_shell_internal(cmd, popen.opts.PIPE)
end

-- Returns false in case of anomaly ('no router' is ok)
local load_router_module = function()
    -- Check both ways to get router module.
    router_module = require 'httpng.router'
    --router_module_c = require 'httpng.router_c'
    local router_module_alt = require 'httpng'.router
    return ((router_module == nil) == (router_module_alt == nil))
end

local ensure_router = function()
::check_router_available::
    if (router_checked) then
        t.skip_if(router_module == nil,
            'This test requires router support')
        return
    end

    if (not load_router_module()) then
        t.skip('This test requires router support but it is broken')
    end

    router_checked = true
    goto check_router_available
end

local function get_new_router(use_c_router)
    ensure_router()
    if use_c_router then
        if (router_module_c == nil) then
            t.skip('no C router available')
        end
        return router_module_c.new()
    end
    return router_module.new()
end

g_shuttle_size = t.group('shuttle_size')

--[[ There is no point testing other values - parameters are automatically
saturated for MIN and MAX, only shuttle_size with Lua handlers is "special"
(MIN_shuttle_size is enough for C handlers)
--]]--
g_shuttle_size.test_small_for_lua = function()
    t.assert_error_msg_content_equals(
        'shuttle_size is too small for Lua handlers',
        http.cfg, { shuttle_size = 64, handler = function() end })
end

g_wrong_config = t.group('wrong_config')

g_wrong_config.test_empty_cfg = function()
    t.assert_error_msg_content_equals('No parameters specified', http.cfg)
end

g_wrong_config_requires_shutdown = t.group('wrong_config_requires_shutdown')
g_wrong_config_requires_shutdown.before_each(ensure_shutdown_works)
g_wrong_config_requires_shutdown.test_no_handlers = function()
    t.assert_error_msg_content_equals('No handlers specified', http.cfg, {})
end

--[[
g_wrong_config.test_c_sites = function()
    t.assert_error_msg_content_equals('c_sites_func must be a function',
        http.cfg, { c_sites_func = 42 })
end

g_wrong_config.test_c_sites_fail = function()
    t.assert_error_msg_content_equals('c_sites_func() failed',
        http.cfg, { c_sites_func = function() error('') end })
end

g_wrong_config.test_c_sites_wrong = function()
    t.assert_error_msg_content_equals('c_sites_func() returned wrong data type',
        http.cfg, { c_sites_func = function() return 42 end })
end
--]]

g_wrong_config.test_wrong_param_type = function()
    t.assert_error_msg_content_equals('parameter threads is not a number',
        http.cfg, { threads = 'test' })
end

g_wrong_config.test_handler_is_not_a_function = function()
    t.assert_error_msg_content_equals(
        'handler is not a function or table (router object)',
        http.cfg, { handler = 42 })
end

g_wrong_config.test_listen_port_invalid = function()
    t.assert_error_msg_content_equals('invalid port specified',
        http.cfg, { handler = function() end, listen = { { port = 77777 } } })
end

g_wrong_config.test_min_proto_version_num = function()
    t.assert_error_msg_content_equals('min_proto_version is not a string',
        http.cfg, { handler = function() end, min_proto_version = 1 })
end

g_wrong_config.test_min_proto_version_invalid = function()
    t.assert_error_msg_content_equals('unknown min_proto_version specified',
        http.cfg, { handler = function() end, min_proto_version = 'ssl2' })
end

g_wrong_config.test_level_nan = function()
    t.assert_error_msg_content_equals('openssl_security_level is not a number',
        http.cfg, { handler = function() end, openssl_security_level = 'ssl' })
end

g_wrong_config.test_level_invalid = function()
    t.assert_error_msg_content_equals('openssl_security_level is invalid',
        http.cfg, { handler = function() end, openssl_security_level = 6 })
end

g_shutdown = t.group('shutdown')

g_shutdown.test_simple_shutdown = function()
    local err = test_shutdown()
    assert(shutdown_support_checked)
    t.fail_if(err ~= nil, err)
end

g_shutdown.test_shutdown_after_wrong_cfg = function()
    t.fail_if(http.shutdown == nil, 'Shutdown is not supported')
    t.assert_error_msg_content_equals('No parameters specified',
        http.cfg)
    t.assert_error_msg_content_equals('Server is not launched',
        http.shutdown)
end

g_shutdown.test_unexpected_shutdown = function()
    t.fail_if(http.shutdown == nil, 'Shutdown is not supported')
    t.assert_error_msg_content_equals('Server is not launched',
        http.shutdown)
end

g_shutdown.test_double_shutdown = function()
    http.cfg({ handler = function() end })
    http.shutdown()
    t.assert_error_msg_content_equals('Server is not launched',
        http.shutdown)
end

local function do_shutdown()
    local ok, err = pcall(http.shutdown)
    if (not ok) then
        print('Warning: shutdown() failed with error "' .. err .. '"')
    end
end

g_bad_handlers = t.group 'bad_handlers'
g_bad_handlers.before_each(ensure_shutdown_works)
g_bad_handlers.after_each(do_shutdown)

local write_handler_launched = false
local bad_write_ok
local bad_write_err
local write_bad_shuttle_ok
local write_bad_shuttle_err
local write_after_write_with_is_last_ok
local write_after_write_with_is_last_err
local write_after_write_with_is_last_result

local write_header_handler_launched = false
local bad_write_header_ok
local bad_write_header_err
local write_header_bad_shuttle_ok
local write_header_bad_shuttle_err
local write_header_invalid_ok
local write_header_invalid_err
local upgrade_to_websocket_bad_shuttle_ok
local upgrade_to_websocket_bad_shuttle_err
local write_first_header_ok
local write_second_header_ok
local write_second_header_err
local upgrade_to_websocket_ok
local upgrade_to_websocket_err
local close_ok
local close_err
local close_bad_shuttle_ok
local close_bad_shuttle_err
local _

local write_handler = function(req, io)
    write_handler_launched = true
    bad_write_ok, bad_write_err = pcall(io.write, io)

    local saved_shuttle = io._shuttle
    io._shuttle = 42
    write_bad_shuttle_ok, write_bad_shuttle_err = pcall(io.write, io, 'a')
    io._shuttle = saved_shuttle

    io:write('foo', true)
    write_after_write_with_is_last_ok, write_after_write_with_is_last_err,
        write_after_write_with_is_last_result = pcall(io.write, io, 'foo')

    io:close()
end

local write_header_handler = function(req, io)
    write_header_handler_launched = true
    bad_write_header_ok, bad_write_header_err = pcall(io.write_header, io)

    local saved_shuttle = io._shuttle
    io._shuttle = 42
    write_header_bad_shuttle_ok, write_header_bad_shuttle_err =
        pcall(io.write_header, io, 200)
    io._shuttle = saved_shuttle

    write_header_invalid_ok, write_header_invalid_err =
        pcall(io.write_header, io, 'a')

    local saved_shuttle = io._shuttle
    io._shuttle = 42
    upgrade_to_websocket_bad_shuttle_ok, upgrade_to_websocket_bad_shuttle_err =
        pcall(io.upgrade_to_websocket, io)
    io._shuttle = saved_shuttle

    write_first_header_ok, _ = pcall(io.write_header, io, 200)
    write_second_header_ok, write_second_header_err =
        pcall(io.write_header, io, 200)

    upgrade_to_websocket_ok, upgrade_to_websocket_err =
        pcall(io.upgrade_to_websocket, io)

    close_ok, close_err = pcall(io.close)

    local saved_shuttle = io._shuttle
    io._shuttle = 42
    close_bad_shuttle_ok, close_bad_shuttle_err = pcall(io.close, io)
    io._shuttle = saved_shuttle

    io:close()
end


local ssl_pairs = require 'tests.ssl_pairs'
local listen_with_single_ssl_pair = {
    tls = {
        ssl_pairs['foo'],
    }
}

local function cfg_bad_handlers(use_tls)
    write_handler_launched = false
    write_header_handler_launched = false
    local router = get_new_router()
    router:route({path = '/write'}, write_handler)
    router:route({path = '/write_header'}, write_header_handler)
    local cfg = { handler = router }
    if (use_tls) then
        cfg.listen = listen_with_single_ssl_pair
    end
    my_http_cfg(cfg)
end

local test_write_params = function(ver, use_tls)
    ensure_popen()
    cfg_bad_handlers(use_tls)
    t.assert(write_handler_launched == false)
    local protocol
    if use_tls then
        protocol = 'https'
    else
        protocol = 'http'
    end
    local ph = my_shell(curl_bin .. ' -k -s ' .. ver ..
        ' ' .. protocol .. '://localhost:3300/write')
    local result = get_client_result(ph)

    t.assert(result == 0, 'http request failed')
    t.assert(write_handler_launched == true, 'Handler was not launched')
    t.assert(bad_write_ok == false,
        'io:write() with invalid parameter set didn\'t fail')
    t.assert_str_matches(bad_write_err, 'Not enough parameters')

    t.assert(write_bad_shuttle_ok == false,
        'io:write() with corrupt io._shuttle didn\'t fail')
    t.assert_str_matches(write_bad_shuttle_err, 'shuttle is invalid')

    t.assert(write_after_write_with_is_last_ok == false or
        write_after_write_with_is_last_err,
        'io:write() after io:write(is_last == true) should either error or return true')
end

g_bad_handlers.test_write_params_http1_insecure = function()
    test_write_params('--http1.1')
end

g_bad_handlers.test_write_params_http1_tls = function()
    test_write_params('--http1.1', true)
end

g_bad_handlers.test_write_params_http2_insecure = function()
    test_write_params('--http2')
end

g_bad_handlers.test_write_params_http2_tls = function()
    test_write_params('--http2', true)
end

local test_write_header_params = function(ver, use_tls)
    ensure_popen()
    cfg_bad_handlers(use_tls)
    t.assert(write_header_handler_launched == false)
    local protocol
    if use_tls then
        protocol = 'https'
    else
        protocol = 'http'
    end
    local ph = my_shell(curl_bin .. ' -k -s ' .. ver ..
        ' -o /dev/null ' ..
        ' ' .. protocol .. '://localhost:3300/write_header')
    local result = get_client_result(ph)
    t.assert(result == 0, 'http request failed')
    t.assert(write_header_handler_launched == true, 'Handler was not launched')
    t.assert(bad_write_header_ok == false,
        'io:write_header() with invalid parameter set didn\'t fail')
    t.assert_str_matches(bad_write_header_err, 'Not enough parameters')

    t.assert(write_header_bad_shuttle_ok == false,
        'io:write_header() with corrupt io._shuttle didn\'t fail')
    t.assert_str_matches(write_header_bad_shuttle_err, 'shuttle is invalid')

    t.assert(write_header_invalid_ok == false,
        'io:write_header() with non-integer HTTP code didn\'t fail')
    t.assert_str_matches(write_header_invalid_err,
        'HTTP code is not an integer')

    t.assert(upgrade_to_websocket_bad_shuttle_ok == false,
        'io:upgrade_to_websocket() with corrupt io._shuttle didn\'t fail')
    if (upgrade_to_websocket_bad_shuttle_err ~= 'attempt to call a nil value'
      and upgrade_to_websocket_bad_shuttle_err ~= 'shuttle is invalid') then
        t.fail('bad shuttle is accepted by upgrade_to_websocket()')
    end

    t.assert(write_first_header_ok == true, 'Valid io:write_header() fail')
    t.assert(write_second_header_ok == false,
        'Second io:write_header() didn\'t fail')
    t.assert_str_matches(write_second_header_err,
        'Handler has already written header')

    t.assert(upgrade_to_websocket_ok == false,
        'io:upgrade_to_websocket() after write_header() didn\'t fail')
    if (upgrade_to_websocket_err ~=
      'Unable to upgrade to WebSockets after sending HTTP headers' and
      upgrade_to_websocket_err ~= 'attempt to call a nil value') then
        t.fail('Unable to upgrade to WebSockets after sending HTTP headers')
    end

    t.assert(close_ok == false,
        'io:close() with invalid parameter set didn\'t fail')
    t.assert_str_matches(close_err, 'Not enough parameters')

    t.assert(close_bad_shuttle_ok == false,
        'io:close() with corrupt io._shuttle didn\'t fail')
    t.assert_str_matches(close_bad_shuttle_err, 'shuttle is invalid')
end

g_bad_handlers.test_write_header_params_http1_insecure = function()
    test_write_header_params '--http1.1'
end

g_bad_handlers.test_write_header_params_http1_tls = function()
    test_write_header_params('--http1.1', true)
end

g_bad_handlers.test_write_header_params_http2_insecure = function()
    test_write_header_params '--http2'
end

g_bad_handlers.test_write_header_params_http2_tls = function()
    test_write_header_params('--http2', true)
end

g_hot_reload = t.group 'hot_reload'
g_hot_reload.before_each(ensure_shutdown_works)
g_hot_reload.after_each(function() pcall(http.shutdown) end)

local foo_handler = function(req, io)
    return { body = 'foo' }
end

local bar_handler = function(req, io)
    return { body = 'bar' }
end

local bar_placeholder_handler = function(req, io)
    return { body = 'bar' }
end

local users_placeholder_handler = function(req, io)
    local body = req:stash('user')
    if (body == nil) then
        body = 'no stash found'
    end
    return { body = body }
end

local empty_handler1 = function(req, io)
end

local empty_handler2 = function(req, io)
    return {}
end

local empty_handler3 = function(req, io)
    return {body = ''}
end

local post_handler = function(req, io)
    if req.method ~= 'POST' then
        return { body = 'only POST method is supported' }
    end
    return { body = req.body }
end

local version_handler_launched = false
local received_http1_req = false
local received_http2_req = false
local check_http_version_handler = function(req, io)
    version_handler_launched = true
    if (req.version_major == 2) then
        received_http2_req = true
    else
        if (req.version_major == 1) then
            received_http1_req = true
        end
    end
    return {body = 'foo'}
end

local get_site_content = function(extra, proto, location, str, timeout)
    ensure_popen()
    local target
    if using_popen() then
        target = ' '
    else
        target = ' -o tmp_curl.txt '
    end
    local timeout_str
    if timeout ~= nil then
        timeout_str = '-m ' .. timeout .. ' '
    else
        timeout_str = ''
    end
    local cmd = curl_bin .. ' -k -s ' .. timeout_str .. extra .. target ..
        proto .. '://' .. location
    local output
    if using_popen() then
        local ph = my_shell_r(cmd)
        local result = ph:wait().exit_code
        output = ph:read()
        assert(result == 0, 'curl failed')
    else
        os.remove('tmp_curl.txt')
        local result = get_client_result(my_shell(cmd))
        assert(result == 0, 'curl failed')
        local file = io.open('tmp_curl.txt')
        if (file == nil) then
            error('nothing read')
        end
        output = file:read('*a')
        file:close()
    end

	return output
end

local check_site_content = function(extra, proto, location, str, timeout)
    local output = get_site_content(extra, proto, location, timeout)

    if (output ~= str) then
        print('Expected: "'..str..'", actual: "'..output..'"')
        assert(output == str, 'Got unexpected response from HTTP(S) server')
    end
end

local http2_support_checked = false
local http2_supported

local test_curl_supports_v2 = function()
    version_handler_launched = false
    received_http1_req = false
    received_http2_req = false
    my_http_cfg{handler = check_http_version_handler}
    check_site_content('--http2', 'http', 'localhost:3300', 'foo')
    assert(version_handler_launched == true)
    if (not received_http1_req and received_http2_req) then
        http2_supported = true
    end
    http2_support_checked = true
    http.shutdown()
end

local ensure_http2 = function()
    if (not http2_support_checked) then
        test_curl_supports_v2()
        assert(http2_support_checked)
    end
    t.skip_if(not http2_supported, 'This test requires HTTP/2 support in curl')
end

local cfg_for_two_sites = function(cfg, first, second, ver, use_tls)
    local proto
    if (use_tls) then
        cfg.listen = listen_with_single_ssl_pair
        proto = 'https'
    else
        proto = 'http'
    end
    my_http_cfg(cfg)
    local location_main = 'localhost:3300/' .. first
    local location_alt = 'localhost:3300/' .. second

    return proto, location_main, location_alt
end

g_hot_reload.test_change_params = function()
    local cfg = {
        handler = write_handler,
        threads = 4,
        max_conn_per_thread = 64,
        shuttle_size = 1024,
        max_body_len = 16 * 1024 * 1024,
        use_body_split = true,
    }

    http.cfg(cfg)

    cfg.max_conn_per_thread = 128
    http.cfg(cfg)

    cfg.max_body_len = 32 * 1024 * 1024
    http.cfg(cfg)

    cfg.use_body_split = false
    http.cfg(cfg)

    cfg.shuttle_size = 2048
    t.assert_error_msg_content_equals(
        "Reconfiguration can't change shuttle_size", http.cfg, cfg)
    cfg.shuttle_size = 1024

    cfg.threads = 5
    http.cfg(cfg)
end

g_hot_reload.test_decrease_threads = function()
    local cfg = {
        handler = foo_handler,
        threads = 4,
    }

    http.cfg(cfg)

    cfg.threads = 3
    http.cfg(cfg)
end

g_hot_reload.test_FLAKY_after_decrease_threads = function()
    local cfg = {
        handler = foo_handler,
        threads = 4,
    }

    http.cfg(cfg)

    cfg.threads = 3
    http.cfg(cfg)

    cfg.threads = 4
    local start = fiber.clock()
::retry::
    local ok, err = pcall(http.cfg, cfg)
    if (not ok) then
        assert(err == 'Unable to reconfigure until threads will shut down')
        assert(fiber.clock() - start < 0.5)
        fiber.sleep(0.01)
        goto retry
    end
end

local curls
g_hot_reload_with_curls = t.group 'hot_reload_with_curls'

local function kill_curls()
    if (curls == nil) then
        return
    end
    if using_popen() then
        for _, curl in pairs(curls) do
            curl:close()
        end
    else
        for _, curl in pairs(curls) do
            os.execute('sh -c "kill ' .. curl .. '" 2>/dev/null')
            pcall(debug_wait_process, curl) -- Avoid zombies.
        end
    end
    curls = nil
end

local function shutdown_and_kill_curls()
    do_shutdown()
    kill_curls()
end

g_hot_reload_with_curls.before_each(ensure_shutdown_works)
g_hot_reload_with_curls.after_each(shutdown_and_kill_curls)

local launch_hungry_curls = function(path, ver, use_tls)
    ensure_popen()
    assert(curls == nil)
    curls = {}
    local curl_count = 48
    local i
    local proto
    if use_tls then
        proto = 'https'
    else
        proto = 'http'
    end
    for i = 1, curl_count do
        curls[#curls + 1] =
            my_shell(curl_bin .. ' ' .. ver ..
                ' -k -s -o /dev/null ' .. proto .. '://' .. path)
    end
end

local test_FLAKY_decrease_stubborn_threads = function(ver, use_tls)
    ensure_popen()
    local cfg = {
        handler = stubborn_handler,
        threads = 2,
    }
    if use_tls then
        cfg.listen = listen_with_single_ssl_pair
    end

    http.cfg(cfg)

    -- We do not (yet?) have API to check that earlier test from combo is done.
    fiber.sleep(0.1)

    launch_hungry_curls('localhost:3300', ver, use_tls)
    fiber.sleep(1)

    cfg.threads = 1
    cfg.listen = nil
    http.cfg(cfg)

    cfg.threads = 2
    local start = fiber.clock()
::retry::
    local ok, err = pcall(http.cfg, cfg)
    assert(not ok, 'httpng.cfg() should fail')
    assert(err == 'Unable to reconfigure until threads will shut down')
    if (fiber.clock() - start >= 1) then
        -- We have waited long enough, this is as it should be.
        http.force_decrease_threads()
        http.cfg(cfg)
        return
    end
    fiber.sleep(0.1)
    goto retry
end

g_hot_reload_with_curls.test_FLAKY_decrease_stubborn_threads_http1_insecure =
        function()
    test_FLAKY_decrease_stubborn_threads '--http1.1'
end

g_hot_reload_with_curls.test_FLAKY_decrease_stubborn_threads_http1_tls =
        function()
    test_FLAKY_decrease_stubborn_threads('--http1.1', true)
end

g_hot_reload_with_curls.test_FLAKY_decrease_stubborn_threads_http2_insecure =
        function()
    ensure_http2()
    test_FLAKY_decrease_stubborn_threads '--http2'
end

g_hot_reload_with_curls.test_FLAKY_decrease_stubborn_threads_http2_tls =
        function()
    ensure_http2()
    test_FLAKY_decrease_stubborn_threads('--http2', true)
end

local test_FLAKY_decrease_stubborn_threads_with_timeout =
        function(ver, use_tls)
    ensure_popen()
    local cfg = {
        handler = stubborn_handler,
        threads = 2,
        thread_termination_timeout = 2,
    }
    if use_tls then
        cfg.listen = listen_with_single_ssl_pair
    end

    my_http_cfg(cfg)

    local curls_start = fiber.clock()
    launch_hungry_curls('localhost:3300', ver, use_tls)
    fiber.sleep(0.1)

    cfg.threads = 1
    cfg.listen = nil
    local start = fiber.clock()
    http.cfg(cfg)

    cfg.threads = 2
::retry::
    local ok, err = pcall(http.cfg, cfg)
    if (ok) then
        local now = fiber.clock()
        if (not(now - start >= cfg.thread_termination_timeout - 0.5)) then
            print('now - start = ', now - start)
            print('now - curls_start = ', now - curls_start)
            print('cfg.thread_termination_timeout = ',
                cfg.thread_termination_timeout)
            error('threads have terminated too early');
        end
        return
    end
    assert(err == 'Unable to reconfigure until threads will shut down')
    assert(fiber.clock() - start < cfg.thread_termination_timeout + 1,
        'threads have terminated too late')
    fiber.sleep(0.05)
    goto retry
end

g_hot_reload_with_curls.test_FLAKY_decrease_stubborn_threads_with_timeout_h1i =
        function()
    test_FLAKY_decrease_stubborn_threads_with_timeout '--http1.1'
end

g_hot_reload_with_curls.test_FLAKY_decrease_stubborn_threads_with_timeout_h1s =
        function()
    test_FLAKY_decrease_stubborn_threads_with_timeout('--http1.1', true)
end

g_hot_reload_with_curls.test_FLAKY_decrease_stubborn_threads_with_timeout_h2i =
        function()
    ensure_http2()
    test_FLAKY_decrease_stubborn_threads_with_timeout '--http2'
end

g_hot_reload_with_curls.test_FLAKY_decrease_stubborn_threads_with_timeout_h2s =
        function()
    ensure_http2()
    test_FLAKY_decrease_stubborn_threads_with_timeout('--http2', true)
end

local test_FLAKY_decrease_not_so_stubborn_thr_with_timeout =
        function(ver, use_tls)
    ensure_popen()
    local cfg = {
        handler = stubborn2_handler,
        threads = 2,
        thread_termination_timeout = 2,
    }
    if use_tls then
        cfg.listen = listen_with_single_ssl_pair
    end

    my_http_cfg(cfg)

    assert(cfg.thread_termination_timeout > 0.1)
    local curls_start = fiber.clock()
    launch_hungry_curls('localhost:3300', ver, use_tls)
    fiber.sleep(0.1)

    cfg.threads = 1
    cfg.listen = nil
    local start = fiber.clock()
    http.cfg(cfg)

    cfg.threads = 2
::retry::
    local ok, err = pcall(http.cfg, cfg)
    if (ok) then
        local now = fiber.clock()
        if (now - start < 0.4) then
            print('now - start = ', now - start)
            print('now - curls_start = ', now - curls_start)
            error('threads have terminated too early');
        end
        assert(fiber.clock() - start < cfg.thread_termination_timeout + 0.5,
            'threads have terminated too late');
        return
    end
    assert(err == 'Unable to reconfigure until threads will shut down')
    assert(fiber.clock() - start < cfg.thread_termination_timeout + 0.5)
    fiber.sleep(0.05)
    goto retry
end

g_hot_reload_with_curls.test_FLAKY_decr_not_so_stubborn_thr_with_timeout_h1i =
        function()
    test_FLAKY_decrease_not_so_stubborn_thr_with_timeout '--http1.1'
end

g_hot_reload_with_curls.test_FLAKY_decr_not_so_stubborn_thr_with_timeout_h1s =
        function()
    test_FLAKY_decrease_not_so_stubborn_thr_with_timeout('--http1.1', true)
end

g_hot_reload_with_curls.test_FLAKY_decr_not_so_stubborn_thr_with_timeout_h2i =
        function()
    ensure_http2()
    test_FLAKY_decrease_not_so_stubborn_thr_with_timeout '--http2'
end

g_hot_reload_with_curls.test_FLAKY_decr_not_so_stubborn_thr_with_timeout_h2s =
        function()
    ensure_http2()
    test_FLAKY_decrease_not_so_stubborn_thr_with_timeout('--http2', true)
end

local alt_foo_handler = function(req, io)
    return { body = 'FOO' }
end

local alt_bar_handler = function(req, io)
    return { body = 'BAR' }
end

local test_replace_handlers = function(ver, use_tls)
    local router = get_new_router()
    router:route({path = '/alt'}, alt_foo_handler)
    router:route({path = '/'}, foo_handler)

    local cfg = {
        handler = router,
        threads = 4,
    }
    local proto, location_main, location_alt =
        cfg_for_two_sites(cfg, '', 'alt', ver, use_tls)

    check_site_content(ver, proto, location_main, 'foo')
    check_site_content(ver, proto, location_alt, 'FOO')

    local router = get_new_router()
    router:route({path = '/alt'}, alt_bar_handler)
    router:route({path = '/'}, bar_handler)

    cfg.handler = router
    cfg.listen = nil
    my_http_cfg(cfg)

    check_site_content(ver, proto, location_main, 'bar')
    check_site_content(ver, proto, location_alt, 'BAR')
end

--[[

--Should finish router support first.

g_hot_reload.test_replace_handlers_http1_insecure = function()
    test_replace_handlers '--http1.1'
end

g_hot_reload.test_replace_handlers_http1_tls = function()
    test_replace_handlers('--http1.1', true)
end

g_hot_reload.test_replace_handlers_http2_insecure = function()
    ensure_http2()
    test_replace_handlers '--http2'
end

g_hot_reload.test_replace_handlers_http2_tls = function()
    ensure_http2()
    test_replace_handlers('--http2', true)
end
--]]

g_hot_reload.test_force_decrease_threads = function()
    t.assert_error_msg_content_equals(
        'Not configured, nothing to terminate', http.force_decrease_threads)
end

g_good_handlers = t.group 'good_handlers'
g_good_handlers.before_each(ensure_shutdown_works)
g_good_handlers.after_each(shutdown_and_kill_curls)

local query_handler = function(req, io)
    if (req.method ~= 'GET') then
        return {status = 500, body = 'Unsupported HTTP method'}
    end
    local payload
    if req.query == 'id=2' then
        payload = req.path .. '+' .. 'good'
    else
        payload = req.path .. '+' .. 'bad'
    end
    return {body = payload}
end

local test_something = function(ver, use_tls, query, handler, expected)
    local cfg = {handler = handler}
    local proto
    if use_tls then
        cfg.listen = listen_with_single_ssl_pair
        proto = 'https'
    else
        proto = 'http'
    end
    my_http_cfg(cfg)
    check_site_content(ver, proto, 'localhost:3300' .. query, expected)
end

local test_some_query = function(ver, use_tls, query, expected)
    test_something(ver, use_tls, query, query_handler, expected)
end

local test_expected_query = function(ver, use_tls)
    test_some_query(ver, use_tls, '/prefix?id=2', '/prefix+good')
end

g_good_handlers.test_expected_query_http1_insecure = function()
    test_expected_query '--http1.1'
end

g_good_handlers.test_expected_query_http1_tls = function()
    test_expected_query('--http1.1', true)
end

g_good_handlers.test_expected_query_http2_insecure = function()
    ensure_http2()
    test_expected_query '--http2'
end

g_good_handlers.test_expected_query_http2_tls = function()
    ensure_http2()
    test_expected_query('--http2', true)
end

local test_unexpected_query = function(ver, use_tls)
    test_some_query(ver, use_tls, '/test?id=3', '/test+bad')
end

g_good_handlers.test_unexpected_query_http1_insecure = function()
    test_unexpected_query '--http1.1'
end

g_good_handlers.test_unexpected_query_http1_tls = function()
    test_unexpected_query('--http1.1', true)
end

g_good_handlers.test_unexpected_query_http2_insecure = function()
    ensure_http2()
    test_unexpected_query '--http2'
end

g_good_handlers.test_unexpected_query_http2_tls = function()
    ensure_http2()
    test_unexpected_query('--http2', true)
end

local test_no_query = function(ver, use_tls)
    test_some_query(ver, use_tls, '', '/+bad')
end

g_good_handlers.test_no_query_http1_insecure = function()
    test_no_query '--http1.1'
end

g_good_handlers.test_no_query_http1_tls = function()
    test_no_query('--http1.1', true)
end

g_good_handlers.test_no_query_http2 = function()
    ensure_http2()
    test_no_query '--http2'
end

g_good_handlers.test_curl_supports_v1 = function()
    version_handler_launched = false
    received_http1_req = false
    received_http2_req = false
    my_http_cfg{handler = check_http_version_handler}
    check_site_content('--http1.1', 'http', 'localhost:3300', 'foo')

    assert(version_handler_launched == true)
    assert(received_http1_req == true)
    assert(received_http2_req == false)
end

g_good_handlers.test_curl_supports_v2 = function()
    if (not http2_support_checked) then
        test_curl_supports_v2()
        assert(http2_support_checked)
    end
    assert(http2_supported,
        'http/2 support in curl is required to test everything fully')
end

g_wrong_config.test_combo3 = function()
    -- Crash or ASAN failure on broken versions.
    ensure_shutdown_works()
    http._cfg_debug{inject_shutdown_error = true}
    pcall(g_shutdown.test_simple_shutdown)
    pcall(g_wrong_config.test_no_handlers)
    http._cfg_debug{inject_shutdown_error = false}
    http.shutdown()
    shutdown_works = true
end

g_wrong_config.test_combo5 = function()
    -- ASAN failure on broken versions.
    ensure_shutdown_works()
    http._cfg_debug{inject_shutdown_error = true}
    pcall(g_shutdown.test_simple_shutdown)
    pcall(g_wrong_config.test_handler_is_not_a_function)
    http._cfg_debug{inject_shutdown_error = false}
    http.shutdown()
    shutdown_works = true
end

local test_cancellation = function(ver, use_tls)
    ensure_can_start_and_kill_processes()
    local cfg = {
        handler = query_handler, -- Almost any (not stubborn!)
        threads = 4,
    }
    if use_tls then
        cfg.listen = listen_with_single_ssl_pair
    end

    http.cfg(cfg)
    for _ = 1, 100 do
        launch_hungry_curls('localhost:3300', ver, use_tls)
        fiber.sleep(0.01)
        kill_curls()
    end
end

g_good_handlers.test_cancellation_http1_tls = function()
    test_cancellation('--http1.1', true)
end

g_good_handlers.test_cancellation_http2_tls = function()
    ensure_http2()
    test_cancellation('--http2', true)
end

g_good_handlers.test_cancellation_http1_insecure = function()
    test_cancellation('--http1.1')
end

g_good_handlers.test_cancellation_http2_insecure = function()
    ensure_http2()
    test_cancellation('--http2')
end

local test_host = function(ver, use_tls)
    local cfg = { handler = function(req) return {body = req.host} end }
    local proto
    if use_tls then
        proto = 'https'
        cfg.listen = {
            tls = { ssl_pairs['foo'], ssl_pairs['bar'] },
            uses_sni = true
        }
    else
        proto = 'http'
    end
    my_http_cfg(cfg)

    check_site_content(ver, proto, 'foo.tarantool.io:3300', 'foo.tarantool.io')
    check_site_content(ver, proto, 'bar.tarantool.io:3300', 'bar.tarantool.io')

    if use_tls then
        local ok, err = pcall(check_site_content, ver, proto,
            'localhost:3300', 'localhost')
        assert(ok == false)
    else
        check_site_content(ver, proto, 'localhost:3300', 'localhost')
    end
end

g_good_handlers.test_host_http1_tls = function()
    test_host('--http1.1', true)
end

g_good_handlers.test_host_http1_insecure = function()
    test_host('--http1.1')
end

g_good_handlers.test_host_http2_tls = function()
    ensure_http2()
    test_host('--http2', true)
end

g_good_handlers.test_host_http2_insecure = function()
    ensure_http2()
    test_host('--http2')
end

local encryption_handler = function(req, io)
    if req.https then
        return {body = 'encrypted'}
    end
    return {body = 'insecure'}
end

local test_req_encryption_info = function(ver)
    my_http_cfg{
        listen = { 8080, { port = 3300, tls = { ssl_pairs['foo'] } } },
        handler = encryption_handler,
    }

    check_site_content(ver, 'http', 'foo.tarantool.io:8080', 'insecure')
    check_site_content(ver, 'https', 'foo.tarantool.io:3300', 'encrypted')
end

g_good_handlers.test_req_encryption_info_http1 = function()
    ensure_http2()
    test_req_encryption_info('--http1.1')
end

g_good_handlers.test_req_encryption_info_http2 = function()
    ensure_http2()
    test_req_encryption_info('--http2')
end

local write_header_handler2 = function(req, io)
    io:write_header(200, nil, 'foo')
end

local write_handler2 = function(req, io)
    io:write('foo')
end

local faulty_handler = function()
    error 'foo'
end

local test_some_handler = function(ver, use_tls, handler)
    test_something(ver, use_tls, '', handler, 'foo')
end

local test_write_header_handler = function(ver, use_tls)
    test_some_handler(ver, use_tls, write_header_handler2)
end

local test_write_handler = function(ver, use_tls)
    test_some_handler(ver, use_tls, write_handler2)
end

local test_faulty_handler = function(ver, use_tls)
    test_something(ver, use_tls, '', faulty_handler,
        'Path handler execution error')
end

g_good_handlers.test_write_header_handler_http2_tls = function()
    ensure_http2()
    test_write_header_handler('--http2', true)
end

g_good_handlers.test_write_header_handler_http2_insecure = function()
    ensure_http2()
    test_write_header_handler('--http2')
end

g_good_handlers.test_write_header_handler_http1_tls = function()
    test_write_header_handler('--http1.1', true)
end

g_good_handlers.test_write_header_handler_http1_insecure = function()
    test_write_header_handler('--http1.1')
end

g_good_handlers.test_write_handler_http2_tls = function()
    ensure_http2()
    test_write_handler('--http2', true)
end

g_good_handlers.test_write_handler_http2_insecure = function()
    ensure_http2()
    test_write_handler('--http2')
end

g_good_handlers.test_write_handler_http1_tls = function()
    test_write_handler('--http1.1', true)
end

g_good_handlers.test_write_handler_http1_insecure = function()
    test_write_handler('--http1.1')
end

g_bad_handlers.test_faulty_handler_http2_tls = function()
    ensure_http2()
    test_faulty_handler('--http2', true)
end

g_bad_handlers.test_faulty_handler_http2_insecure = function()
    ensure_http2()
    test_faulty_handler('--http2')
end

g_bad_handlers.test_faulty_handler_http1_tls = function()
    test_faulty_handler('--http1.1', true)
end

g_bad_handlers.test_faulty_handler_http1_insecure = function()
    test_faulty_handler('--http1.1')
end

local headers_to_send
local send_headers_handler_return = function(req)
    if req.method ~= 'GET' then
        return { body = 'only GET method is supported' }
    end
    return { headers = headers_to_send, body = 'foo' }
end

local send_headers_handler_write = function(req, io)
    if req.method ~= 'GET' then
        return { body = 'only GET method is supported' }
    end
    io.headers = headers_to_send
    io:write('foo')
end

local send_headers_handler_write_header = function(req, io)
    if req.method ~= 'GET' then
        return { body = 'only GET method is supported' }
    end
    io:write_header(200, headers_to_send)
    io:write('foo')
end

local send_headers_handler_write_header_implicit = function(req, io)
    if req.method ~= 'GET' then
        return { body = 'only GET method is supported' }
    end
    io.headers = headers_to_send
    io:write_header(200)
    io:write('foo')
end

local test_sending_headers_internal = function(ver, use_tls)
    local expected = 'Path handler execution error'
    test_something(ver, use_tls, '', send_headers_handler_return, 'foo')
    test_something(ver, use_tls, '', send_headers_handler_write, expected)
    test_something(ver, use_tls, '', send_headers_handler_write_header,
        expected)
    test_something(ver, use_tls, '',
        send_headers_handler_write_header_implicit, expected)
end

local test_sending_headers = function(ver, use_tls)
    headers_to_send = 'bad'
    test_sending_headers_internal(ver, use_tls)
    headers_to_send = { 'bad' }
    test_sending_headers_internal(ver, use_tls)
    headers_to_send = { { 'worse' } }
    test_sending_headers_internal(ver, use_tls)
end

g_bad_handlers.test_sending_headers_http1_insecure = function()
    test_sending_headers('--http1.1')
end

g_bad_handlers.test_sending_headers_http2_insecure = function()
    test_sending_headers('--http2')
end

--[[
g_bad_handlers.test_sending_headers_http1_tls = function()
    test_sending_headers('--http1.1', true)
end

g_bad_handlers.test_sending_headers_http2_tls = function()
    test_sending_headers('--http2', true)
end
--]]

g_hot_reload.test_min_proto = function()
    local cfg = { handler = function() end, min_proto_version = 'tls1.2'}
    http.cfg(cfg)
    cfg.min_proto_version = 'tls1.3'
    t.assert_error_msg_content_equals(
        "min_proto_version can't be changed on reconfiguration", http.cfg, cfg)
end

g_hot_reload.test_security_level = function()
    local cfg = { handler = function() end, openssl_security_level = 1}
    http.cfg(cfg)
    cfg.openssl_security_level = 2
    t.assert_error_msg_content_equals(
        "openssl_security_level can't be changed on reconfiguration",
        http.cfg, cfg)
end

g_hot_reload.test_listen_change = function()
    local cfg = { handler = function() end, listen = 3300}
    http.cfg(cfg)
    cfg.listen = 8080
    t.assert_error_msg_content_equals(
        "listen can't be changed on reconfiguration",
        http.cfg, cfg)
end

g_hot_reload.test_null_threads = function()
    http.cfg{threads = 4, handler = function() end}
    http.cfg{threads = box.NULL}
end

g_hot_reload.test_null_thread_termination_timeout = function()
    http.cfg{thread_termination_timeout = 5, handler = function() end}
    http.cfg{thread_termination_timeout = box.NULL}
end

g_hot_reload.test_null_shuttle_size = function()
    http.cfg{shuttle_size = 1024, handler = function() end}
    t.assert_error_msg_content_equals(
        "Reconfiguration can't change shuttle_size",
        http.cfg, {shuttle_size = box.NULL})
end

g_hot_reload.test_change_min_proto_version = function()
    http.cfg{min_proto_version = 'tls1.3', handler = function() end}
    t.assert_error_msg_content_equals(
        "min_proto_version can't be changed on reconfiguration",
        http.cfg, {min_proto_version = 'tls1.2'})
end

g_hot_reload.test_null_min_proto_version = function()
    http.cfg{min_proto_version = 'tls1.3', handler = function() end}
    t.assert_error_msg_content_equals(
        "min_proto_version can't be changed on reconfiguration",
        http.cfg, {min_proto_version = box.NULL})
end

g_hot_reload.test_null_security_level = function()
    http.cfg{openssl_security_level = 5, handler = function() end}
    t.assert_error_msg_content_equals(
        "openssl_security_level can't be changed on reconfiguration",
        http.cfg, {openssl_security_level = box.NULL})
end

g_hot_reload.test_null_listen = function()
    http.cfg{listen = 8080, handler = function() end}
    t.assert_error_msg_content_equals(
        "listen can't be changed on reconfiguration",
        http.cfg, {listen = box.NULL})
end

g_hot_reload.test_reload_file = function()
    local filename = 'tmp_reload_test'
    local filename_ext = filename .. '.lua'
    local file = io.open(filename_ext, 'w')
    file:write(
    [[
    local handler = function(req, io)
        return { body = 'foo' }
    end

    return {handler = handler}
    ]]
    )
    file:close()

    http.cfg{handler = require(filename).handler}
    check_site_content('', 'http', 'localhost:3300', 'foo')
    local file = io.open(filename_ext, 'w')
    file:write(
    [[
    local handler = function(req, io)
        return { body = 'bar' }
    end

    return {handler = handler}
    ]]
    )
    file:close()
    package.loaded[filename] = nil
    http.cfg{handler = require(filename).handler}
    check_site_content('', 'http', 'localhost:3300', 'bar')
end

g_good_handlers.test_router_available = function()
    if (not load_router_module()) then
        error('Router support is broken')
    end
    t.fail_if(router_module == nil, 'No router module available')
end

local test_router_basics = function(use_c_router)
    local router = get_new_router(use_c_router)
    router:route({path = '/foo'}, foo_handler)
    router:route({path = '/bar'}, bar_handler)

    http.cfg{handler = router}
    check_site_content('', 'http', 'localhost:3300/foo', 'foo')
    check_site_content('', 'http', 'localhost:3300/bar', 'bar')
    check_site_content('', 'http', 'localhost:3300/foo?query=0', 'foo')
    check_site_content('', 'http', 'localhost:3300/bar?query=0', 'bar')
end

g_good_handlers.test_router_basics_lua = function()
    test_router_basics()
end

g_good_handlers.test_router_basics_c = function()
    test_router_basics(true)
end

local test_router_configured = function(use_c_router)
    local router = get_new_router(use_c_router)
    router:route({path = '/foo'}, foo_handler)

    http.cfg{handler = router}
    check_site_content('', 'http', 'localhost:3300/foo', 'foo')
    check_site_content('', 'http', 'localhost:3300/bar', 'not found')

    router:route({path = '/bar'}, bar_handler)
    check_site_content('', 'http', 'localhost:3300/bar', 'bar')
end

g_good_handlers.test_router_configured_lua = function()
    test_router_configured()
end

g_good_handlers.test_router_configured_c = function()
    test_router_configured(true)
end

local test_router_collect = function(use_c_router)
    local router = get_new_router(use_c_router)
    router:route({path = '/foo'}, foo_handler)
    router:route({path = '/bar'}, bar_handler)

    http.cfg{handler = router}
    check_site_content('', 'http', 'localhost:3300/foo', 'foo')
    check_site_content('', 'http', 'localhost:3300/bar', 'bar')

    http.shutdown()
    router = nil
    collectgarbage()
end

g_good_handlers.test_router_collect_lua = function()
    test_router_collect()
end

g_good_handlers.test_router_collect_c = function()
    test_router_collect(true)
end

local test_router_placeholder_regular = function(use_c_router)
    local router = get_new_router(use_c_router)
    router:route({path = '/foo'}, foo_handler)
    router:route({path = '/:bar'}, bar_placeholder_handler)
    router:route({path = '/users/:user'}, users_placeholder_handler)

    http.cfg{handler = router}
    check_site_content('', 'http', 'localhost:3300/foo', 'foo')
    check_site_content('', 'http', 'localhost:3300/bar', 'bar')
    check_site_content('', 'http', 'localhost:3300/stuff', 'bar')
    check_site_content('', 'http', 'localhost:3300/users/one', 'one')
    check_site_content('', 'http', 'localhost:3300/users/two', 'two')
    check_site_content('', 'http', 'localhost:3300/users/two/more', 'not found')
    check_site_content('', 'http', 'localhost:3300/foo?query=0', 'foo')
    check_site_content('', 'http', 'localhost:3300/bar?query=0', 'bar')
    check_site_content('', 'http', 'localhost:3300/stuff?query=0', 'bar')
    check_site_content('', 'http', 'localhost:3300/users/one?query=0', 'one')
    check_site_content('', 'http', 'localhost:3300/users/two?query=0', 'two')
    check_site_content('', 'http', 'localhost:3300/users/two/more?query=0', 'not found')
end

local test_router_placeholder_regular_angle1 = function(use_c_router)
    local router = get_new_router(use_c_router)
    router:route({path = '/foo'}, foo_handler)
    router:route({path = '/<:bar>'}, bar_placeholder_handler)
    router:route({path = '/users/<:user>'}, users_placeholder_handler)

    http.cfg{handler = router}
    check_site_content('', 'http', 'localhost:3300/foo', 'foo')
    check_site_content('', 'http', 'localhost:3300/bar', 'bar')
    check_site_content('', 'http', 'localhost:3300/stuff', 'bar')
    check_site_content('', 'http', 'localhost:3300/users/one', 'one')
    check_site_content('', 'http', 'localhost:3300/users/two', 'two')
    check_site_content('', 'http', 'localhost:3300/users/two/more', 'not found')
    check_site_content('', 'http', 'localhost:3300/foo?query=0', 'foo')
    check_site_content('', 'http', 'localhost:3300/bar?query=0', 'bar')
    check_site_content('', 'http', 'localhost:3300/stuff?query=0', 'bar')
    check_site_content('', 'http', 'localhost:3300/users/one?query=0', 'one')
    check_site_content('', 'http', 'localhost:3300/users/two?query=0', 'two')
    check_site_content('', 'http', 'localhost:3300/users/two/more?query=0', 'not found')
end

local test_router_placeholder_regular_angle2 = function(use_c_router)
    local router = get_new_router(use_c_router)
    router:route({path = '/foo'}, foo_handler)
    router:route({path = '/<bar>'}, bar_placeholder_handler)
    router:route({path = '/users/<user>'}, users_placeholder_handler)

    http.cfg{handler = router}
    check_site_content('', 'http', 'localhost:3300/foo', 'foo')
    check_site_content('', 'http', 'localhost:3300/bar', 'bar')
    check_site_content('', 'http', 'localhost:3300/stuff', 'bar')
    check_site_content('', 'http', 'localhost:3300/users/one', 'one')
    check_site_content('', 'http', 'localhost:3300/users/two', 'two')
    check_site_content('', 'http', 'localhost:3300/users/two/more', 'not found')
    check_site_content('', 'http', 'localhost:3300/foo?query=0', 'foo')
    check_site_content('', 'http', 'localhost:3300/bar?query=0', 'bar')
    check_site_content('', 'http', 'localhost:3300/stuff?query=0', 'bar')
    check_site_content('', 'http', 'localhost:3300/users/one?query=0', 'one')
    check_site_content('', 'http', 'localhost:3300/users/two?query=0', 'two')
    check_site_content('', 'http', 'localhost:3300/users/two/more?query=0', 'not found')
end

g_good_handlers.test_router_placeholder_regular_lua = function()
    test_router_placeholder_regular()
end

g_good_handlers.test_router_placeholder_regular_c = function()
    test_router_placeholder_regular(true)
end

g_good_handlers.test_router_placeholder_regular_angle1_lua = function()
    test_router_placeholder_regular_angle1()
end

g_good_handlers.test_router_placeholder_regular_angle1_c = function()
    test_router_placeholder_regular_angle1(true)
end

g_good_handlers.test_router_placeholder_regular_angle2_lua = function()
    test_router_placeholder_regular_angle2()
end

g_good_handlers.test_router_placeholder_regular_angle2_c = function()
    test_router_placeholder_regular_angle2(true)
end

local test_router_placeholder_wildcard = function(use_c_router)
    local router = get_new_router(use_c_router)
    router:route({path = '/foo'}, foo_handler)
    router:route({path = '/users/*user'}, users_placeholder_handler)
    router:route({path = '/*bar'}, bar_placeholder_handler)

    http.cfg{handler = router}
    check_site_content('', 'http', 'localhost:3300/foo', 'foo')
    check_site_content('', 'http', 'localhost:3300/bar', 'bar')
    check_site_content('', 'http', 'localhost:3300/stuff', 'bar')
    check_site_content('', 'http', 'localhost:3300/users/one', 'one')
    check_site_content('', 'http', 'localhost:3300/users/two', 'two')
    check_site_content('', 'http', 'localhost:3300/users/two/more', 'two/more')
    check_site_content('', 'http', 'localhost:3300/foo?query=0', 'foo')
    check_site_content('', 'http', 'localhost:3300/bar?query=0', 'bar')
    check_site_content('', 'http', 'localhost:3300/stuff?query=0', 'bar')
    check_site_content('', 'http', 'localhost:3300/users/one?query=0', 'one')
    check_site_content('', 'http', 'localhost:3300/users/two?query=0', 'two')
    check_site_content('', 'http', 'localhost:3300/users/two/more?query=0', 'two/more')
end

local test_router_placeholder_wildcard_angle = function(use_c_router)
    local router = get_new_router(use_c_router)
    router:route({path = '/foo'}, foo_handler)
    router:route({path = '/users/<*user>'}, users_placeholder_handler)
    router:route({path = '/<*bar>'}, bar_placeholder_handler)

    http.cfg{handler = router}
    check_site_content('', 'http', 'localhost:3300/foo', 'foo')
    check_site_content('', 'http', 'localhost:3300/bar', 'bar')
    check_site_content('', 'http', 'localhost:3300/stuff', 'bar')
    check_site_content('', 'http', 'localhost:3300/users/one', 'one')
    check_site_content('', 'http', 'localhost:3300/users/two', 'two')
    check_site_content('', 'http', 'localhost:3300/users/two/more', 'two/more')
    check_site_content('', 'http', 'localhost:3300/foo?query=0', 'foo')
    check_site_content('', 'http', 'localhost:3300/bar?query=0', 'bar')
    check_site_content('', 'http', 'localhost:3300/stuff?query=0', 'bar')
    check_site_content('', 'http', 'localhost:3300/users/one?query=0', 'one')
    check_site_content('', 'http', 'localhost:3300/users/two?query=0', 'two')
    check_site_content('', 'http', 'localhost:3300/users/two/more?query=0', 'two/more')
end

g_good_handlers.test_router_placeholder_wildcard_lua = function()
    test_router_placeholder_wildcard()
end

g_good_handlers.test_router_placeholder_wildcard_c = function()
    test_router_placeholder_wildcard(true)
end

g_good_handlers.test_router_placeholder_wildcard_angle_lua = function()
    test_router_placeholder_wildcard_angle()
end

g_good_handlers.test_router_placeholder_wildcard_angle_c = function()
    test_router_placeholder_wildcard_angle(true)
end

local test_empty_response = function(handler, ver, use_tls)
    local cfg = {handler = handler}
    local proto
    if (use_tls) then
        cfg.listen = listen_with_single_ssl_pair
        proto = 'https'
    else
        proto = 'http'
    end
    http.cfg(cfg)
    check_site_content(ver, proto, 'foo.tarantool.io:3300/', '', 3)
end

g_good_handlers.test_empty_response1_http1_insecure = function()
    test_empty_response(empty_handler1, '--http1.1')
end

g_good_handlers.test_empty_response1_http1_tls = function()
    test_empty_response(empty_handler1, '--http1.1', true)
end

g_good_handlers.test_empty_response1_http2_insecure = function()
    test_empty_response(empty_handler1, '--http2')
end

g_good_handlers.test_empty_response1_http2_tls = function()
    test_empty_response(empty_handler1, '--http2', true)
end

g_good_handlers.test_empty_response2_http1_insecure = function()
    test_empty_response(empty_handler2, '--http1.1')
end

g_good_handlers.test_empty_response2_http1_tls = function()
    test_empty_response(empty_handler2, '--http1.1', true)
end

g_good_handlers.test_empty_response2_http2_insecure = function()
    test_empty_response(empty_handler2, '--http2')
end

g_good_handlers.test_empty_response2_http2_tls = function()
    test_empty_response(empty_handler2, '--http2', true)
end

g_good_handlers.test_empty_response3_http1_insecure = function()
    test_empty_response(empty_handler3, '--http1.1')
end

g_good_handlers.test_empty_response3_http1_tls = function()
    test_empty_response(empty_handler3, '--http1.1', true)
end

g_good_handlers.test_empty_response3_http2_insecure = function()
    test_empty_response(empty_handler3, '--http2')
end

g_good_handlers.test_empty_response3_http2_tls = function()
    test_empty_response(empty_handler3, '--http2', true)
end

local test_post = function(ver, use_tls)
    local cfg = {handler = post_handler}
    local proto
    if (use_tls) then
        cfg.listen = listen_with_single_ssl_pair
        proto = 'https'
    else
        proto = 'http'
    end
    http.cfg(cfg)

    local test = 'The Matrix has you'
    check_site_content(ver .. ' -d "' .. test .. '" ', proto,
        'localhost:3300', test)
end

g_good_handlers.test_post_http2_tls = function()
    test_post('--http2', true)
end

g_good_handlers.test_post_http1_tls = function()
    test_post('--http1.1', true)
end

g_good_handlers.test_post_http2_insecure = function()
    test_post('--http2')
end

g_good_handlers.test_post_http1_insecure = function()
    test_post('--http1.1')
end

local req_headers_handler = function(req, io)
    if req.method ~= 'GET' then
        return { body = 'only GET method is supported' }
    end
    io:write('Headers:\n')
    for k, v in pairs(req.headers) do
        if (string.sub(k, 1, 2) == 'x-') then
            io:write(k .. ': ' .. v .. '\n')
        end
    end
end

local test_req_headers = function(ver, use_tls)
    local cfg = { handler = req_headers_handler }
    local proto
    if (use_tls) then
        cfg.listen = listen_with_single_ssl_pair
        proto = 'https'
    else
        proto = 'http'
    end
    my_http_cfg(cfg)

    local h1 = 'x-short-test-header: x'
    local h2 = 'x-long-test-header: There is no spoon'
    check_site_content(ver .. ' -H "' .. h1 .. '" -H "' .. h2 .. '"', proto,
        'localhost:3300', 'Headers:\n' .. h1 .. '\n' .. h2 .. '\n')
end

g_good_handlers.test_req_headers_http1_insecure = function()
    test_req_headers('--http1.1')
end

g_good_handlers.test_req_headers_http2_insecure = function()
    test_req_headers('--http2')
end

g_good_handlers.test_req_headers_http1_tls = function()
    test_req_headers('--http1.1', true)
end

g_good_handlers.test_req_headers_http2_tls = function()
    test_req_headers('--http2', true)
end

local response_headers = {
    ['x-foo1'] = 'bar',
    ['x-foo2'] = 'very long string',
}

local test_resp_headers_internal = function(handler, ver, use_tls)
    local cfg = { handler = handler }
    local proto
    if (use_tls) then
        cfg.listen = listen_with_single_ssl_pair
        proto = 'https'
    else
        proto = 'http'
    end
    headers_to_send = response_headers
    my_http_cfg(cfg)

    local content = get_site_content(ver .. ' -i', proto, 'localhost:3300')
    local lines = {}
    for s in content:gmatch("[^\r\n]+") do
        table.insert(lines, s)
    end

    local found = {}
    local count = 0
    for _, s in ipairs(lines) do
        local k, v = string.match(s, "([0-9a-zA-Z%-]+): (.*)")
        if k then
            if response_headers[k] then
                assert(not found[k], 'duplicated header detected')
                found[k] = true
                count = count + 1
                assert(response_headers[k] == v, 'header is corrupted')
            end
        end
    end

    local expected_count = 0
    for _ in pairs(response_headers) do
        expected_count = expected_count + 1
    end
    assert(count == expected_count, 'not all expected headers are present')
end

local test_resp_headers = function(ver, use_tls)
    test_resp_headers_internal(send_headers_handler_return, ver, use_tls)
    test_resp_headers_internal(send_headers_handler_write, ver, use_tls)
    test_resp_headers_internal(send_headers_handler_write_header, ver, use_tls)
    test_resp_headers_internal(send_headers_handler_write_header_implicit, ver, use_tls)
end

g_good_handlers.test_resp_headers_http1_insecure = function()
    test_resp_headers('--http1.1')
end

g_good_handlers.test_resp_headers_http2_insecure = function()
    test_resp_headers('--http2')
end

--[[
g_good_handlers.test_resp_headers_http1_tls = function()
    test_resp_headers('--http1.1', true)
end

g_good_handlers.test_resp_headers_http2_tls = function()
    test_resp_headers('--http2', true)
end
--]]

local put_echo_handler = function(req, io)
    if req.method ~= 'PUT' then
        return { body = 'only PUT method is supported' }
    end
    return { body = req.body }
end

local test_put = function(ver, use_tls)
    local cfg = { handler = put_echo_handler }
    local proto
    if (use_tls) then
        cfg.listen = listen_with_single_ssl_pair
        proto = 'https'
    else
        proto = 'http'
    end
    my_http_cfg(cfg)

    local put = 'This is a payload'

    local file = assert(io.open('tmp_put.bin', 'wb'))
    assert(file ~= nil, "Can't create temp file")
    file:write(put)
    file:close()

    local output = get_site_content(ver .. ' -T tmp_put.bin',
        proto, 'localhost:3300')
    os.remove('tmp_put.bin')
    if (output ~= put) then
        print('Expected: "' .. put .. '", actual: "' .. output .. '"')
        assert(output == put, 'Got unexpected response from HTTP(S) server')
    end
end

g_good_handlers.test_put_http1_insecure = function()
    test_put('--http1.1')
end

g_good_handlers.test_put_http2_insecure = function()
    test_put('--http2')
end

g_good_handlers.test_put_http1_tls = function()
    test_put('--http1.1', true)
end

g_good_handlers.test_put_http2_tls = function()
    test_put('--http2', true)
end

local chunked_handler = function(req, io)
    if req.method ~= 'GET' then
        return { body = 'only GET method is supported' }
    end
    io:write('1')
    io:write('2')
    io:write('3')
    return { body = 'boom' }
end

local test_chunked = function(ver, use_tls)
    local cfg = { handler = chunked_handler }
    local proto
    if (use_tls) then
        cfg.listen = listen_with_single_ssl_pair
        proto = 'https'
    else
        proto = 'http'
    end
    my_http_cfg(cfg)

    check_site_content(ver, proto, 'localhost:3300', '123boom')
end

g_good_handlers.test_chunked_http1_insecure = function()
    test_chunked('--http1.1')
end

g_good_handlers.test_chunked_http2_insecure = function()
    test_chunked('--http2')
end

g_good_handlers.test_chunked_http1_tls = function()
    test_chunked('--http1.1', true)
end

g_good_handlers.test_chunked_http2_tls = function()
    test_chunked('--http2', true)
end

local content_length_handler_explicit = function(req, io)
    if req.method ~= 'GET' then
        return { body = 'only GET method is supported' }
    end
    io.headers = { ['Content-Length'] = 6 }
    io:write('foo')
    return { body = 'bar' }
end

local content_length_handler_implicit = function(req, io)
    if req.method ~= 'GET' then
        return { body = 'only GET method is supported' }
    end
    return { body = 'foobar' }
end

local test_content_length_internal = function(handler, ver, use_tls)
    local cfg = { handler = handler }
    local proto
    if (use_tls) then
        cfg.listen = listen_with_single_ssl_pair
        proto = 'https'
    else
        proto = 'http'
    end
    my_http_cfg(cfg)

    local content = get_site_content(ver .. ' -i', proto, 'localhost:3300')
    local lines = {}
    for s in content:gmatch("[^\r\n]+") do
        table.insert(lines, s)
    end

    local found = {}
    local count = 0
    local found = false
    local headers_found = false
    local remainder = nil
    for _, s in ipairs(lines) do
        local k, v = string.match(s, "([0-9a-zA-Z%-]+): (.*)")
        if k ~= nil and string.lower(k) == 'content-length' then
            assert(not found)
            found = true
            assert(v == '6', 'Content-Length is invalid')
        else
            if k == nil then
                if headers_found then
                    remainder = s
                    goto done
                end
            else
                headers_found = true
            end
        end
    end

::done::
    assert(remainder == 'foobar', 'Body is broken')
    assert(found, 'No Content-Length header found')
end

local test_content_length_explicit = function(ver, use_tls)
    test_content_length_internal(content_length_handler_explicit, ver, use_tls)
end

local test_content_length_implicit = function(ver, use_tls)
    test_content_length_internal(content_length_handler_implicit, ver, use_tls)
end

g_good_handlers.test_content_length_explicit_http1_tls = function()
    test_content_length_explicit('--http1.1', true)
end

g_good_handlers.test_content_length_explicit_http2_tls = function()
    test_content_length_explicit('--http2', true)
end

g_good_handlers.test_content_length_explicit_http1_insecure = function()
    test_content_length_explicit('--http1.1')
end

g_good_handlers.test_content_length_explicit_http2_insecure = function()
    test_content_length_explicit('--http2 --http2-prior-knowledge')
end

g_good_handlers.test_content_length_implicit_http1_tls = function()
    test_content_length_implicit('--http1.1', true)
end

g_good_handlers.test_content_length_implicit_http2_tls = function()
    test_content_length_implicit('--http2', true)
end

g_good_handlers.test_content_length_implicit_http1_insecure = function()
    test_content_length_implicit('--http1.1')
end

g_good_handlers.test_content_length_implicit_http2_insecure = function()
    test_content_length_implicit('--http2 --http2-prior-knowledge')
end
