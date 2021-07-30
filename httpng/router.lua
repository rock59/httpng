local function is_valid_placeholder_name_char_relaxed(n)
    return (string.match(string.char(n), '[a-zA-Z0-9_]') ~= nil)
end

local function stash(stashes, pattern, name_offset, name_offset_after_last,
        path, match_offset, match_offset_after_last)
    stashes[string.sub(pattern, name_offset, name_offset_after_last)] =
        string.sub(path, match_offset, match_offset_after_last)
end

local function check_match(stashes, path, v)
    local pattern = v.entry.path
    if (path == pattern) then
        return true
    end

    local pattern_len1 = #pattern + 1
    local path_len1 = #path + 1
    local pattern_offset = 1
    local path_offset = 1

::again::
    local pattern_char = pattern:byte(pattern_offset)
    if (pattern_char == path:byte(path_offset)) then
            pattern_offset = pattern_offset + 1
            path_offset = path_offset + 1
            if (pattern_offset >= pattern_len1) then
                return (path_offset >= path_len1)
            end
            if (path_offset >= path_len1) then
                return false
            end
    else
        if (not (string.match(string.char(pattern_char), '[<:%*]'))) then
            return false
        end
        local match_offset = path_offset
        -- Determine placeholder name.
        local name_offset = 1
        local is_wildcard
        if (pattern_char == string.byte('<')) then
                pattern_offset = pattern_offset + 1
                pattern_char = pattern:byte(pattern_offset)
                if (pattern_char == string.byte('*')) then
                        is_wildcard = true
                        pattern_offset = pattern_offset + 1
                        name_offset = pattern_offset
                else
                        is_wildcard = false
                        if (pattern_char == string.byte(':')) then
                            pattern_offset = pattern_offset + 1
                            name_offset = pattern_offset
                        else
                            name_offset = pattern_offset
                            pattern_offset = pattern_offset + 1
                        end
                end
        else
                pattern_offset = pattern_offset + 1
                name_offset = pattern_offset;
                is_wildcard = (pattern_char == string.byte('*'))
        end

        local name_offset_after_last
    ::another_iteration_for_name::
        pattern_char = pattern:byte(pattern_offset)
        pattern_offset = pattern_offset + 1
        if (not is_valid_placeholder_name_char_relaxed(pattern_char)) then
            if (pattern_char == string.byte('>')) then
                pattern_offset = pattern_offset + 1
            end
            name_offset_after_last = pattern_offset
            goto try_to_match_placeholder
        end

        if (pattern_offset >= pattern_len1) then
            name_offset_after_last = pattern_offset
            goto try_to_match_placeholder
        end

        goto another_iteration_for_name

    ::try_to_match_placeholder::
        if (is_wildcard) then
            if (pattern_offset >= pattern_len1) then
                stash(stashes, pattern, name_offset, name_offset_after_last,
                    path, match_offset, path_len1)
                return true
            end
            -- Search for next pattern char.
            local pattern_char = pattern:byte(pattern_offset)
        ::another_iteration_for_pattern::
            if (path:byte(path_offset) == pattern_char) then
                stash(stashes, pattern, name_offset, name_offset_after_last,
                    path, match_offset, path_len1)
                pattern_offset = pattern_offset + 1
                path_offset = path_offset + 1
                if (path_offset >= path_len1) then
                    return (pattern_offset >= pattern_len1)
                end
                goto done_with_pattern
            end

            path_offset = path_offset + 1
            if (path_offset >= path_len1) then
                return false
            end
            goto another_iteration_for_pattern
        ::done_with_pattern::
        else
            if (pattern_offset >= pattern_len1) then
                -- Nothing to match, but '/' is not allowed (except at the end)
            ::another_iteration_for_last_pattern::
                if (path:byte(path_offset) == string.byte('/')) then
                    if (path_offset + 1 ~= path_len1) then
                        return false
                    end

                    stash(stashes, pattern, name_offset,
                        name_offset_after_last, path, match_offset, path_len1)
                    return true
                end

                path_offset = path_offset + 1
                if (path_offset >= path_len1) then
                    stash(stashes, pattern, name_offset, name_offset_after_last,
                        path, match_offset, path_len1)
                    return true
                end

                goto another_iteration_for_last_pattern
            end

            -- Search for '/' or next pattern char.
            local pattern_char = pattern:byte(pattern_offset)
            -- FIXME: Optimize for (pattern_char == '/')?
        ::another_iteration_for_pattern::
            local path_char = path:byte(path_offset)
            if (path_char == string.byte('/') or path_char == pattern_char) then
                stash(stashes, pattern, name_offset, name_offset_after_last,
                    path, match_offset, path_len1)
                goto done_with_pattern
            end
            path_offset = path_offset + 1
            if (path_offset >= path_len1) then
                return false
            end
            goto another_iteration_for_pattern
        ::done_with_pattern::
        end
    end
    goto again
end

local function handle(unused, req, io)
    local routes = req._used_router._routes
    req._stashes = {}
    local stashes = req._stashes
    local query = req.query
    local query_cut_len = (query == nil) and -1 or -(#query + 2)
    local path = req.path:sub(1, query_cut_len)
    for _, v in pairs(routes) do
        if check_match(stashes, path, v) then
            return v.handler(req, io)
        end
    end
    return { status = 404, body = 'not found' }
end

local function route(self, entry, handler)
    self._routes[#self._routes + 1] = { entry = entry, handler = handler }
end

local function new_router()
    local router = { route = route, _routes = {} }
    local mt = { __call = handle }
    setmetatable(router, mt)
    return router
end

return { new = new_router, route = route }
