local function handle(unused, req, io)
    local routes = req._used_router._routes
    for _, v in pairs(routes) do
        if (req.path == v.entry.path) then
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
