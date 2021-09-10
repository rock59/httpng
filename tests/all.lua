local t = require('luatest')
local http = require 'httpng'
local fiber = require 'fiber'
local ssl_pairs = require 'tests.ssl_pairs'
local popen
pcall(function() popen = require 'popen' end)
local curl_bin = 'curl'
local curl_silent = ' -s '
local debug_wait_process = require 'test_helpers'.debug_wait_process
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
        if (os.execute('./process_helper ' .. cmd) ~= 0) then
            return nil
        end
    ::retry_pid::
        local file = t.assert(io.open 'tmp_pid.txt')
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

local get_site_content = function(extra, proto, location, timeout)
    ensure_can_start_and_kill_processes()
    if proto == 'http' then
        t.skip('insecure HTTP is not yet supported')
    end
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

::again::
    local cmd = curl_bin .. ' -k ' .. curl_silent .. timeout_str ..
        extra .. target .. proto .. '://' .. location
    local output
    if using_popen() then
        local ph = my_shell_r(cmd)
        local result = ph:wait().exit_code
        output = ph:read()
        t.assert_equals(result, 0, 'curl failed')
    else
        os.remove('tmp_curl.txt')
        local result = get_client_result(my_shell(cmd))
        t.assert_equals(result, 0, 'curl failed')
        local file = io.open('tmp_curl.txt')
        if (file == nil) then
            if extra:find('--http1.1') then
                extra = extra:gsub('--http1.1', '')
                goto again
            end
            output = ''
        else
            output = file:read('*a')
            file:close()
        end
    end
    return output
end

local check_site_content = function(extra, proto, location, str, timeout)
    local output = get_site_content(extra, proto, location, timeout)

    t.assert_equals(output, str, 'Got unexpected response from HTTP(S) server')
end

local http2_support_checked = false
local http2_supported

local test_curl_supports_v2 = function()
    version_handler_launched = false
    received_http1_req = false
    received_http2_req = false
    real_handler = check_http_version_handler
    my_http_cfg{ handler = universal_handler }
    local ok, err =
        pcall(check_site_content, '--http2', 'https', 'localhost:3300', 'foo')
    if not version_handler_launched then
        return 'curl does not work properly'
    end
    if ok and (not received_http1_req and received_http2_req) then
        http2_supported = true
    end
    http2_support_checked = true
end

local ensure_http2 = function()
    if (not http2_support_checked) then
        local result = test_curl_supports_v2()
        t.skip_if(result, result)
        t.assert(http2_support_checked)
    end
    t.skip_if(not http2_supported, 'This test requires HTTP/2 support in curl')
end

local foo_handler = function(req, io)
    return { body = 'foo' }
end

local is_forced_http1_1_supported_checked = false
local is_forced_http1_1_supported_result

local is_forced_http1_1_supported = function()
    if is_forced_http1_1_supported_checked then
        return is_forced_http1_1_supported_result
    end

    -- TODO: change to cfg() + GET + shutdown()
    local cmd = curl_bin .. ' --http1.1 http://localhost:1023'
    local result
    if using_popen() then
        local ph = my_shell_r(cmd)
        result = ph:wait().exit_code
    else
        result = get_client_result(my_shell(cmd))
    end
    is_forced_http1_1_supported_result = (result == 7)
    is_forced_http1_1_supported_checked = true
    return is_forced_http1_1_supported_result
end

local http1_1_ver_str = function()
    local ver
    if is_forced_http1_1_supported() then
        return '--http1.1'
    end
    return ''
end

g_bad_handlers = t.group 'bad_handlers'

local write_handler_launched = false
local bad_write_ok
local bad_write_err
local write_bad_shuttle_ok
local write_bad_shuttle_err

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

local function cfg_bad_handlers(use_tls)
    write_handler_launched = false
    write_header_handler_launched = false
    local cfg = { handler = universal_handler }
    if (use_tls) then
        cfg.listen = listen_with_single_ssl_pair
    end
    my_http_cfg(cfg)
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

local test_write_header_params = function(ver, use_tls)
    ensure_can_start_and_kill_processes()
    if not use_tls then
        t.skip('insecure HTTP is not yet supported')
    end
    real_handler = write_header_handler
    cfg_bad_handlers(use_tls)
    t.assert(write_header_handler_launched == false)
    local protocol
    if use_tls then
        protocol = 'https'
    else
        protocol = 'http'
    end
    local ph = my_shell(curl_bin .. ' -k ' .. curl_silent .. ver ..
        ' -o /dev/null ' ..
        ' ' .. protocol .. '://localhost:3300')
    local result = get_client_result(ph)
    t.assert(result == 0, 'http request failed')
    t.assert(write_header_handler_launched == true, 'Handler was not launched')
    t.assert(bad_write_header_ok == false,
        "io:write_header() with invalid parameter set didn't fail")
    t.assert_str_matches(bad_write_header_err, 'Not enough parameters')

    t.assert(write_header_bad_shuttle_ok == false,
        "io:write_header() with corrupt io._shuttle didn't fail")
    t.assert_str_matches(write_header_bad_shuttle_err, 'shuttle is invalid')

    t.assert(write_header_invalid_ok == false,
        "io:write_header() with non-integer HTTP code didn't fail")
    t.assert_str_matches(write_header_invalid_err,
        'HTTP code is not an integer')

    t.assert(upgrade_to_websocket_bad_shuttle_ok == false,
        "io:upgrade_to_websocket() with corrupt io._shuttle didn't fail")
    if (upgrade_to_websocket_bad_shuttle_err ~= 'attempt to call a nil value'
      and upgrade_to_websocket_bad_shuttle_err ~= 'shuttle is invalid') then
        t.fail('bad shuttle is accepted by upgrade_to_websocket()')
    end

    t.assert(write_first_header_ok == true, 'Valid io:write_header() fail')
    t.assert(write_second_header_ok == false,
        "Second io:write_header() didn't fail")
    t.assert_str_matches(write_second_header_err,
        'Handler has already written header')

    t.assert(upgrade_to_websocket_ok == false,
        "io:upgrade_to_websocket() after write_header() didn't fail")
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
    test_write_header_params(http1_1_ver_str())
end

g_bad_handlers.test_write_header_params_http1_tls = function()
    test_write_header_params(http1_1_ver_str(), true)
end

g_bad_handlers.test_write_header_params_http2_insecure = function()
    ensure_http2()
    test_write_header_params '--http2'
end

g_bad_handlers.test_write_header_params_http2_tls = function()
    ensure_http2()
    test_write_header_params('--http2', true)
end

local write_handler = function(req, io)
    write_handler_launched = true
    bad_write_ok, bad_write_err = pcall(io.write, io)

    local saved_shuttle = io._shuttle
    io._shuttle = 42
    write_bad_shuttle_ok, write_bad_shuttle_err = pcall(io.write, io, 'a')
    io._shuttle = saved_shuttle

    io:close()
end

local test_write_params = function(ver, use_tls)
    ensure_can_start_and_kill_processes()
    if not use_tls then
        t.skip('insecure HTTP is not yet supported')
    end
    real_handler = write_handler
    cfg_bad_handlers(use_tls)
    t.assert(write_handler_launched == false)
    local protocol
    if use_tls then
        protocol = 'https'
    else
        protocol = 'http'
    end
    local ph = my_shell(curl_bin .. ' -k ' .. curl_silent .. ver ..
        ' ' .. protocol .. '://localhost:3300')
    local result = get_client_result(ph)

    t.assert(result == 0, 'http request failed')
    t.assert(write_handler_launched == true, 'Handler was not launched')
    t.assert(bad_write_ok == false,
        "io:write() with invalid parameter set didn't fail")
    t.assert_str_matches(bad_write_err, 'Not enough parameters')

    t.assert(write_bad_shuttle_ok == false,
        "io:write() with corrupt io._shuttle didn't fail")
    t.assert_str_matches(write_bad_shuttle_err, 'shuttle is invalid')
end

g_bad_handlers.test_write_params_http1_insecure = function()
    test_write_params(http1_1_ver_str())
end

g_bad_handlers.test_write_params_http1_tls = function()
    test_write_params(http1_1_ver_str(), true)
end

g_bad_handlers.test_write_params_http2_insecure = function()
    ensure_http2()
    test_write_params('--http2')
end

g_bad_handlers.test_write_params_http2_tls = function()
    ensure_http2()
    test_write_params('--http2', true)
end

g_good_handlers = t.group 'good_handlers'

g_good_handlers.test_curl_supports_v1 = function()
    version_handler_launched = false
    received_http1_req = false
    received_http2_req = false
    real_handler = check_http_version_handler
    my_http_cfg{handler = universal_handler}
    check_site_content(http1_1_ver_str(), 'https', 'localhost:3300', 'foo')

    t.assert_equals(version_handler_launched, true)
    t.assert_equals(received_http1_req, true)
    t.assert_equals(received_http2_req, false)
end

g_good_handlers.test_curl_supports_v2 = function()
    if (not http2_support_checked) then
        local result = test_curl_supports_v2()
        t.assert_equals(result, nil, result)
        t.assert(http2_support_checked)
    end
    t.assert(http2_supported,
        'http/2 support in curl is required to test everything fully')
end

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
    local cfg = { handler = universal_handler }
    local proto
    if use_tls then
        cfg.listen = listen_with_single_ssl_pair
        proto = 'https'
    else
        proto = 'http'
    end
    real_handler = handler
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
    test_expected_query(http1_1_ver_str())
end

g_good_handlers.test_expected_query_http1_tls = function()
    test_expected_query(http1_1_ver_str(), true)
end

g_good_handlers.test_expected_query_http2_insecure = function()
    ensure_http2()
    test_expected_query '--http2'
end

local test_host = function(ver, use_tls)
    local cfg = { handler = universal_handler }
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
    real_handler = function(req) return {body = req.host} end
    my_http_cfg(cfg)

    check_site_content(ver, proto, 'foo.tarantool.io:3300', 'foo.tarantool.io')
    check_site_content(ver, proto, 'bar.tarantool.io:3300', 'bar.tarantool.io')

    do
        check_site_content(ver, proto, 'localhost:3300', 'localhost')
    end
end

g_good_handlers.test_host_http1_tls = function()
    test_host(http1_1_ver_str(), true)
end

g_good_handlers.test_host_http1_insecure = function()
    test_host(http1_1_ver_str())
end

g_good_handlers.test_host_http2_tls = function()
    ensure_http2()
    test_host('--http2', true)
end

g_good_handlers.test_host_http2_insecure = function()
    ensure_http2()
    test_host('--http2')
end

local write_header_handler2 = function(req, io)
    io:write_header(200, nil)
    io:write('foo')
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
    test_write_header_handler(http1_1_ver_str(), true)
end

g_good_handlers.test_write_header_handler_http1_insecure = function()
    test_write_header_handler(http1_1_ver_str())
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
    test_write_handler(http1_1_ver_str(), true)
end

g_good_handlers.test_write_handler_http1_insecure = function()
    test_write_handler(http1_1_ver_str())
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
    test_faulty_handler(http1_1_ver_str(), true)
end

g_bad_handlers.test_faulty_handler_http1_insecure = function()
    test_faulty_handler(http1_1_ver_str())
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
    test_sending_headers(http1_1_ver_str())
end

g_bad_handlers.test_sending_headers_http2_insecure = function()
    ensure_http2()
    test_sending_headers('--http2')
end

g_bad_handlers.test_sending_headers_http1_tls = function()
    test_sending_headers(http1_1_ver_str(), true)
end

g_bad_handlers.test_sending_headers_http2_tls = function()
    ensure_http2()
    test_sending_headers('--http2', true)
end

local test_empty_response = function(handler, ver, use_tls)
    local cfg = { handler = universal_handler }
    local proto
    if (use_tls) then
        cfg.listen = listen_with_single_ssl_pair
        proto = 'https'
    else
        proto = 'http'
    end
    real_handler = handler
    my_http_cfg(cfg)
    check_site_content(ver, proto, 'localhost:3300', '', 3)
end

local empty_handler1 = function(req, io)
end

local empty_handler2 = function(req, io)
    return {}
end

local empty_handler3 = function(req, io)
    return {body = ''}
end

g_good_handlers.test_empty_response1_http1_insecure = function()
    test_empty_response(empty_handler1, http1_1_ver_str())
end

g_good_handlers.test_empty_response1_http1_tls = function()
    test_empty_response(empty_handler1, http1_1_ver_str(), true)
end

g_good_handlers.test_empty_response1_http2_insecure = function()
    ensure_http2()
    test_empty_response(empty_handler1, '--http2')
end

g_good_handlers.test_empty_response1_http2_tls = function()
    ensure_http2()
    test_empty_response(empty_handler1, '--http2', true)
end

g_good_handlers.test_empty_response2_http1_insecure = function()
    test_empty_response(empty_handler2, http1_1_ver_str())
end

g_good_handlers.test_empty_response2_http1_tls = function()
    test_empty_response(empty_handler2, http1_1_ver_str(), true)
end

g_good_handlers.test_empty_response2_http2_insecure = function()
    ensure_http2()
    test_empty_response(empty_handler2, '--http2')
end

g_good_handlers.test_empty_response2_http2_tls = function()
    ensure_http2()
    test_empty_response(empty_handler2, '--http2', true)
end

g_good_handlers.test_empty_response3_http1_insecure = function()
    test_empty_response(empty_handler3, http1_1_ver_str())
end

g_good_handlers.test_empty_response3_http1_tls = function()
    test_empty_response(empty_handler3, http1_1_ver_str(), true)
end

g_good_handlers.test_empty_response3_http2_insecure = function()
    ensure_http2()
    test_empty_response(empty_handler3, '--http2')
end

g_good_handlers.test_empty_response3_http2_tls = function()
    ensure_http2()
    test_empty_response(empty_handler3, '--http2', true)
end

local post_handler = function(req, io)
    if req.method ~= 'POST' then
        return { body = 'only POST method is supported' }
    end
    return { body = req.body }
end

local test_post = function(ver, use_tls)
    local cfg = { handler = universal_handler }
    local proto
    if (use_tls) then
        cfg.listen = listen_with_single_ssl_pair
        proto = 'https'
    else
        proto = 'http'
    end
    real_handler = post_handler
    my_http_cfg(cfg)

    local test = 'The Matrix has you'
    check_site_content(ver .. ' -d "' .. test .. '" ', proto,
        'localhost:3300', test)
end

g_good_handlers.test_post_http2_tls = function()
    ensure_http2()
    test_post('--http2', true)
end

g_good_handlers.test_post_http1_tls = function()
    test_post(http1_1_ver_str(), true)
end

g_good_handlers.test_post_http2_insecure = function()
    ensure_http2()
    test_post('--http2')
end

g_good_handlers.test_post_http1_insecure = function()
    test_post(http1_1_ver_str())
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
    local cfg = { handler = universal_handler }
    local proto
    if (use_tls) then
        cfg.listen = listen_with_single_ssl_pair
        proto = 'https'
    else
        proto = 'http'
    end
    real_handler = req_headers_handler
    my_http_cfg(cfg)

    local h1 = 'x-short-test-header: x'
    local h2 = 'x-long-test-header: There is no spoon'
    check_site_content(ver .. ' -H "' .. h1 .. '" -H "' .. h2 .. '"', proto,
        'localhost:3300', 'Headers:\n' .. h1 .. '\n' .. h2 .. '\n')
end

g_good_handlers.test_req_headers_http1_insecure = function()
    test_req_headers(http1_1_ver_str())
end

g_good_handlers.test_req_headers_http2_insecure = function()
    ensure_http2()
    test_req_headers('--http2')
end

g_good_handlers.test_req_headers_http1_tls = function()
    test_req_headers(http1_1_ver_str(), true)
end

g_good_handlers.test_req_headers_http2_tls = function()
    ensure_http2()
    test_req_headers('--http2', true)
end

local response_headers = {
    ['x-foo1'] = 'bar',
    ['x-foo2'] = 'very long string',
}

local test_resp_headers_internal = function(handler, ver, use_tls)
    local cfg = { handler = universal_handler }
    local proto
    if (use_tls) then
        cfg.listen = listen_with_single_ssl_pair
        proto = 'https'
    else
        proto = 'http'
    end
    headers_to_send = response_headers
    real_handler = handler
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
                t.assert_not(found[k], 'duplicated header detected')
                found[k] = true
                count = count + 1
                t.assert_equals(response_headers[k], v, 'header is corrupted')
            end
        end
    end

    local expected_count = 0
    for _ in pairs(response_headers) do
        expected_count = expected_count + 1
    end
    t.assert_equals(count, expected_count,
        'not all expected headers are present')
end

local test_resp_headers = function(ver, use_tls)
    test_resp_headers_internal(send_headers_handler_return, ver, use_tls)
    test_resp_headers_internal(send_headers_handler_write, ver, use_tls)
    test_resp_headers_internal(send_headers_handler_write_header, ver, use_tls)
    test_resp_headers_internal(send_headers_handler_write_header_implicit, ver, use_tls)
end

g_good_handlers.test_resp_headers_http1_insecure = function()
    test_resp_headers(http1_1_ver_str())
end

g_good_handlers.test_resp_headers_http2_insecure = function()
    ensure_http2()
    test_resp_headers('--http2')
end

g_good_handlers.test_resp_headers_http1_tls = function()
    test_resp_headers(http1_1_ver_str(), true)
end

g_good_handlers.test_resp_headers_http2_tls = function()
    ensure_http2()
    test_resp_headers('--http2', true)
end

local put_echo_handler = function(req, io)
    if req.method ~= 'PUT' then
        return { body = 'only PUT method is supported' }
    end
    return { body = req.body }
end

local test_put = function(ver, use_tls)
    local cfg = { handler = universal_handler }
    local proto
    if (use_tls) then
        cfg.listen = listen_with_single_ssl_pair
        proto = 'https'
    else
        proto = 'http'
    end
    real_handler = put_echo_handler
    my_http_cfg(cfg)

    local put = 'This is a payload'

    local file = t.assert(io.open('tmp_put.bin', 'wb'))
    t.assert(file ~= nil, "Can't create temp file")
    file:write(put)
    file:close()

    local output = get_site_content(ver .. ' -T tmp_put.bin',
        proto, 'localhost:3300')
    os.remove('tmp_put.bin')
    t.assert_equals(output, put, 'Got unexpected response from HTTP(S) server')
end

g_good_handlers.test_put_http1_insecure = function()
    test_put(http1_1_ver_str())
end

g_good_handlers.test_put_http2_insecure = function()
    ensure_http2()
    test_put('--http2')
end

g_good_handlers.test_put_http1_tls = function()
    test_put(http1_1_ver_str(), true)
end

g_good_handlers.test_put_http2_tls = function()
    ensure_http2()
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
    local cfg = { handler = universal_handler }
    local proto
    if (use_tls) then
        cfg.listen = listen_with_single_ssl_pair
        proto = 'https'
    else
        proto = 'http'
    end
    real_handler = chunked_handler
    my_http_cfg(cfg)

    check_site_content(ver, proto, 'localhost:3300', '123boom')
end

g_good_handlers.test_chunked_http1_insecure = function()
    test_chunked(http1_1_ver_str())
end

g_good_handlers.test_chunked_http2_insecure = function()
    ensure_http2()
    test_chunked('--http2')
end

g_good_handlers.test_chunked_http1_tls = function()
    test_chunked(http1_1_ver_str(), true)
end

g_good_handlers.test_chunked_http2_tls = function()
    ensure_http2()
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
    local cfg = { handler = universal_handler }
    local proto
    if (use_tls) then
        cfg.listen = listen_with_single_ssl_pair
        proto = 'https'
    else
        proto = 'http'
    end
    real_handler = handler
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
            t.assert_not(found)
            found = true
            t.assert_equals(v, '6', 'Content-Length is invalid')
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
    t.assert_equals(remainder, 'foobar', 'Body is broken')
    t.assert(found, 'No Content-Length header found')
end

local test_content_length_explicit = function(ver, use_tls)
    test_content_length_internal(content_length_handler_explicit, ver, use_tls)
end

local test_content_length_implicit = function(ver, use_tls)
    test_content_length_internal(content_length_handler_implicit, ver, use_tls)
end

g_good_handlers.test_content_length_explicit_http1_tls = function()
    test_content_length_explicit(http1_1_ver_str(), true)
end

g_good_handlers.test_content_length_explicit_http2_tls = function()
    ensure_http2()
    test_content_length_explicit('--http2', true)
end

g_good_handlers.test_content_length_explicit_http1_insecure = function()
    test_content_length_explicit(http1_1_ver_str())
end

g_good_handlers.test_content_length_explicit_http2_insecure = function()
    ensure_http2()
    test_content_length_explicit('--http2 --http2-prior-knowledge')
end

g_good_handlers.test_content_length_implicit_http1_tls = function()
    test_content_length_implicit(http1_1_ver_str(), true)
end

g_good_handlers.test_content_length_implicit_http2_tls = function()
    ensure_http2()
    test_content_length_implicit('--http2', true)
end

g_good_handlers.test_content_length_implicit_http1_insecure = function()
    test_content_length_implicit(http1_1_ver_str())
end

g_good_handlers.test_content_length_implicit_http2_insecure = function()
    ensure_http2()
    test_content_length_implicit('--http2 --http2-prior-knowledge')
end
