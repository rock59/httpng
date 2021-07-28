#include "router.h"
#include <lauxlib.h>
#include <module.h>

typedef enum {
	/* is_error() relies on sign (optimization). */
	R_MATCH = 0,
	R_NOT_MATCH = 1,
	R_ERROR = -1,
} match_result_t;

typedef struct {
	size_t len;
	int handler_ref;
	bool has_placeholders; /* Performance optimization. */
} entry_header_t; /* String follows, not terminated, aligned. */

typedef struct httpng_router {
	void *entries;
	size_t entries_len;
	size_t entries_count;
} router_t;

typedef unsigned short counter_t;

typedef struct {
	header_offset_t start; /* In path[]. */
	header_offset_t end; /* In path[]. */
	header_offset_t name_len;
	/* name[name_len], NULL-terminated,
	 * aligned to sizeof(header_offset_t). */
} stash_entry_t;

typedef struct {
	counter_t *counter_ptr;
	char *pos;
	unsigned bytes_remain;
} state_t;

typedef struct shuttle shuttle_t;

static int metatable_ref = LUA_REFNIL;

/* Launched in HTTP(S) server thread. */
static inline bool
is_error(match_result_t result)
{
	return (result < 0);
}

/* Launched in HTTP(S) server thread. */
static inline bool
is_valid_placeholder_name_char_relaxed(char c)
{
	return ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
		c == '_' || (c >= 'A' && c <= 'Z'));
}

/* Launched in HTTP(S) server thread.
 * Returns R_MATCH on success, R_ERROR on error
 * (just performance optimization). */
static match_result_t
stash(state_t *state, const char *pattern, size_t name_offset,
	size_t name_offset_after_last, const char *path, size_t match_offset,
	size_t match_offset_after_last)
{
	const unsigned name_len = name_offset_after_last - name_offset;
	const unsigned name_len_aligned =
		(name_len + 1 + sizeof(header_offset_t)
		- 1) & ~(sizeof(header_offset_t) - 1);
	if (sizeof(stash_entry_t) + name_len_aligned > state->bytes_remain)
		return R_ERROR;
	stash_entry_t *const entry = (stash_entry_t *)state->pos;
	entry->start = match_offset;
	entry->end = match_offset_after_last;
	entry->name_len = name_len;
	char *const target = (char *)(entry + 1);
	memcpy(target, &pattern[name_offset], name_len);
	target[name_len] = 0;
	++*state->counter_ptr;
	state->pos += sizeof(stash_entry_t) + name_len_aligned;
	state->bytes_remain -= sizeof(stash_entry_t) + name_len_aligned;
	return R_MATCH;
}

/* Launched in HTTP(S) server thread. */
static match_result_t
check_placeholder_match(state_t *state, const char *pattern,
	size_t pattern_len, const char *path, size_t path_len)
{
	size_t pattern_offset = 0, path_offset = 0;

	while (1) {
		char pattern_char = *(pattern + pattern_offset);
		if (pattern_char == *(path + path_offset)) {
			++pattern_offset;
			++path_offset;
			if (pattern_offset >= pattern_len)
				return (path_offset >= path_len);
			if (path_offset >= path_len)
				return R_NOT_MATCH;
		} else {
			switch (pattern_char) {
			case '<':
			case ':':
			case '*':
				break;
			default:
				return R_NOT_MATCH;
			}

			const size_t match_offset = path_offset;
			/* Determine placeholder name. */
			size_t name_offset;
			bool is_wildcard;
			if (pattern_char == '<') {
				++pattern_offset;
				pattern_char = *(pattern + pattern_offset);
				if (pattern_char == '*') {
					is_wildcard = true;
					name_offset = ++pattern_offset;
				} else {
					is_wildcard = false;
					name_offset = (pattern_char == ':') ?
						++pattern_offset :
						pattern_offset++;
				}
			} else {
				name_offset = ++pattern_offset;
				is_wildcard = (pattern_char == '*');
			}
			size_t name_offset_after_last;
			while (1) {
				pattern_char = *(pattern + pattern_offset++);
				if (!is_valid_placeholder_name_char_relaxed(
				    pattern_char)) {
					name_offset_after_last =
						(pattern_char == '>') ?
							pattern_offset++ :
							pattern_offset;
					break;
				}
				if (pattern_offset >= pattern_len) {
					name_offset_after_last = pattern_offset;
					break;
				}
			}

			/* Try to match placeholder. */
			if (is_wildcard) {
				if (pattern_offset >= pattern_len)
					return stash(state, pattern,
						name_offset,
						name_offset_after_last,
						path, match_offset, path_len);
				/* Search for next pattern char. */
				const char pattern_char =
					*(pattern + pattern_offset);
				while (1) {
					if (*(path + path_offset) ==
					    pattern_char) {
						if (stash(state, pattern,
						    name_offset,
						    name_offset_after_last,
						    path, match_offset,
						    path_len) != R_MATCH)
							return R_ERROR;
						++pattern_offset;
						++path_offset;
						if (path_offset >= path_len)
							return (pattern_offset
								>= pattern_len)
								? R_MATCH :
								R_NOT_MATCH;
						break;
					}
					if (++path_offset >= path_len)
						return R_NOT_MATCH;
				}
			} else {
				if (pattern_offset >= pattern_len) {
					/* Nothing to match, but '/' is not
					 * allowed (except at the end). */
					while (1) {
						if (*(path + path_offset)
						    == '/') {
							if (path_offset + 1
							    != path_len)
							return R_NOT_MATCH;

							return stash(state,
								pattern,
								name_offset,
							name_offset_after_last,
								path,
								match_offset,
								path_len);
						}
						if (++path_offset >= path_len)
							return stash(state,
								pattern,
								name_offset,
							name_offset_after_last,
								path,
								match_offset,
								path_len);
					}
				}
				/* This is essentially "else" but w/o indent.
				 * Search for '/' or next pattern char. */
				const char pattern_char =
					*(pattern + pattern_offset);
				/* FIXME: Optimize for (pattern_char == '/')? */
				while (1) {
					const char path_char =
						*(path + path_offset);
					if (path_char == '/' ||
					    path_char == pattern_char) {
						if (stash(state, pattern,
						    name_offset,
						    name_offset_after_last,
						    path, match_offset,
						    path_len) != R_MATCH)
							return R_ERROR;
						break;
					}
					if (++path_offset >= path_len)
						return R_NOT_MATCH;
				}
			}
		}
	}
	return R_NOT_MATCH;
}

/* Launched in HTTP(S) server thread. */
static int
req_handler(lua_h2o_handler_t *self, h2o_req_t *req)
{
	shuttle_t *const shuttle = prepare_shuttle2(req);
	if (shuttle == NULL)
		return 0;
	state_t state;
	unsigned router_data_len_max;
	state.counter_ptr = get_router_data(shuttle, &router_data_len_max);
	if (router_data_len_max < sizeof(*state.counter_ptr)) {
	too_small_shuttle_size:
		/* shuttle_size is too small.
		 * FIXME: Check that in route()?
		 * Such check is only possible if router
		 * has already been attached to the server.
		 * Maybe extra API to check this on router
		 * attachment? */
		free_shuttle_with_anchor(shuttle);
		req->res.status = 500;
		req->res.reason = "not enough buffer space";
		h2o_send_inline(req, H2O_STRLIT("not enough buffer space"));
		return 0;
	}
	state.pos = (char *)(state.counter_ptr + 1);
	*state.counter_ptr = 0;
	state.bytes_remain =
		router_data_len_max - sizeof(*state.counter_ptr);
	router_t *const router = self->c_handler_param;

	const size_t path_only_len = (req->query_at == SIZE_MAX) ?
		req->path.len : req->query_at;

	const char *const path = req->path.base;
	const size_t count = router->entries_count;
	const char *pos = router->entries;
	size_t idx;
	for (idx = 0; idx < count; ++idx) {
		const entry_header_t *const entry = (entry_header_t *)pos;
		if (entry->has_placeholders) {
			const match_result_t result =
				check_placeholder_match(&state,
					(char *)(entry + 1),
					entry->len, path, path_only_len);
			if (result == R_MATCH)
				return lua_req_handler_ex(self->path, req,
					shuttle, entry->handler_ref,
					router_data_len_max
					- state.bytes_remain, LUA_REFNIL);
			if (is_error(result))
				goto too_small_shuttle_size;
		} else {
		    if (path_only_len == entry->len &&
			    !memcmp(path, entry + 1, path_only_len))
			return lua_req_handler_ex(self->path, req, shuttle,
				entry->handler_ref, 0, LUA_REFNIL);
		}
		pos += (entry->len + sizeof(entry_header_t) * 2
			- 1) / sizeof(entry_header_t) * sizeof(entry_header_t);
	}

	free_shuttle_with_anchor(shuttle);
	/* FIXME: Customizable 404 payload. */
	req->res.status = 404;
	req->res.reason = "not found";
	h2o_send_inline(req, H2O_STRLIT("not found"));
	return 0;
}

/* Launched in TX thread. */
static void
fill_router_data(lua_State *L, const char *path, unsigned data_len,
	void *data)
{
	/* We should create table with stashes in the Lua table at -1. */
	if (data_len == 0)
		return;
	counter_t remain = *(counter_t *)data;
	if (remain == 0)
		return;
	lua_createtable(L, 0, remain);
	const stash_entry_t *entry = (stash_entry_t *)((counter_t *)data + 1);
	do {
		lua_pushlstring(L, &path[entry->start],
			entry->end - entry->start);
		lua_setfield(L, -2, (const char *)(entry + 1));
		const unsigned name_len_aligned =
			(entry->name_len + 1 + sizeof(header_offset_t)
			- 1) & ~(sizeof(header_offset_t) - 1);
		entry = (stash_entry_t *)((char *)entry +
			sizeof(stash_entry_t) + name_len_aligned);
	} while (--remain);
	lua_setfield(L, -2, "_stashes");
}

/* Launched in TX thread. */
static bool
find_placeholders(const char *path, size_t len)
{
	/* FIXME: Add COMPLETE pattern sanity check, return enum. */
	size_t offset = 0;
	while (offset < len) {
		switch (*(path + offset)) {
		case '<':
		case ':':
		case '*':
			return true;
		}
		++offset;
	}
	return false;
}

/* Launched in TX thread. */
static int
route(lua_State *L)
{
	/* Lua parameters: self, entry, handler. */
	enum {
		LUA_STACK_IDX_SELF = 1,
		LUA_STACK_IDX_ENTRY = 2,
		LUA_STACK_IDX_HANDLER = 3,
	};
	const char *lerr = NULL;
	if (lua_gettop(L) < 3) {
		lerr = "Not enough parameters";
		goto error_not_enough_parameters;
	}

	if (lua_type(L, LUA_STACK_IDX_SELF) != LUA_TTABLE) {
		lerr = "first parameter (self) is not a table";
		goto error_self_not_table;
	}
	lua_getfield(L, LUA_STACK_IDX_SELF, ROUTER_C_HANDLER_PARAM_FIELD_NAME);
	if (!lua_isuserdata(L, -1)) {
		lerr = "first parameter (self) content is invalid";
		goto error_wrong_self;
	}
	router_t *const self = (router_t *)lua_touserdata(L, -1);
	lua_pop(L, 1);

	if (lua_type(L, LUA_STACK_IDX_HANDLER) != LUA_TFUNCTION) {
		lerr = "handler is not a function";
		goto error_wrong_handler;
	}
	const int ref = luaL_ref(L, LUA_REGISTRYINDEX);

	lua_getfield(L, LUA_STACK_IDX_ENTRY, "path");
	if (lua_type(L, -1) != LUA_TSTRING) {
		lerr = "path is not a string";
		goto error_wrong_path;
	}

	size_t len;
	const char *const path = lua_tolstring(L, -1, &len);
	const size_t new_entries_len = self->entries_len +
		(len + sizeof(entry_header_t) * 2 - 1) /
		sizeof(entry_header_t) * sizeof(entry_header_t);
	void *const new_entries = malloc(new_entries_len);
	if (new_entries == NULL) {
		lerr = "no memory";
		goto no_memory;
	}

	void *const old_entries = self->entries;
	memcpy(new_entries, old_entries, self->entries_len);
	entry_header_t *const new_entry =
		(entry_header_t *)((char *)new_entries + self->entries_len);
	new_entry->len = len;
	new_entry->handler_ref = ref;
	new_entry->has_placeholders = find_placeholders(path, len);
	memcpy(new_entry + 1, path, len);
	self->entries = new_entries;
	self->entries_len = new_entries_len;
	++self->entries_count;

	__sync_synchronize();
	httpng_flush_requests_to_router();
	free(old_entries);
	return 0;

no_memory:
error_wrong_path:
	luaL_unref(L, LUA_REGISTRYINDEX, ref);
error_wrong_handler:
error_self_not_table:
error_wrong_self:
error_not_enough_parameters:
	assert(lerr != NULL);
	return luaL_error(L, lerr);
}

/* Launched in TX thread. */
static int
collect(lua_State *L)
{
	/* Lua parameters: self. */
	enum {
		LUA_STACK_IDX_SELF = 1,
	};
	const char *lerr = NULL;
	if (lua_gettop(L) < 1) {
		lerr = "Not enough parameters";
		goto error_not_enough_parameters;
	}

	if (!lua_isuserdata(L, LUA_STACK_IDX_SELF)) {
		lerr = "first parameter (self) is invalid";
		goto error_wrong_self;
	}
	router_t *const self =
		(router_t *)lua_touserdata(L, LUA_STACK_IDX_SELF);

	const size_t count = self->entries_count;
	const void *pos = self->entries;
	size_t idx;
	for (idx = 0; idx < count; ++idx) {
		const entry_header_t *const entry = pos;
		luaL_unref(L, LUA_REGISTRYINDEX, entry->handler_ref);
		pos = (char *)pos + (entry->len + sizeof(entry_header_t) * 2
			- 1) / sizeof(entry_header_t) * sizeof(entry_header_t);
	}
	free(self->entries);
	return 0;

error_wrong_self:
error_not_enough_parameters:
	assert(lerr != NULL);
	return luaL_error(L, lerr);
}

/* Launched in TX thread. */
static int
create_router(lua_State *L)
{
	lua_createtable(L, 0, 5);
	lua_pushcfunction(L, route);
	lua_setfield(L, -2, "route");
	lua_pushlightuserdata(L, req_handler);
	lua_setfield(L, -2, ROUTER_C_HANDLER_FIELD_NAME);
	lua_pushlightuserdata(L, fill_router_data);
	lua_setfield(L, -2, ROUTER_FILL_ROUTER_DATA_FIELD_NAME);

	router_t *const router = lua_newuserdata(L, sizeof(*router));
	if (router == NULL)
		return luaL_error(L, "no memory");
	lua_rawgeti(L, LUA_REGISTRYINDEX, metatable_ref);
	lua_setmetatable(L, -2);
	router->entries = NULL;
	router->entries_len = 0;
	router->entries_count = 0;
	lua_setfield(L, -2, ROUTER_C_HANDLER_PARAM_FIELD_NAME);
	return 1;
}

static const struct luaL_Reg mylib[] = {
	{"new", create_router},
	{"route", route},
	{NULL, NULL}
};

int
luaopen_httpng_router_c(lua_State *L)
{
	lua_createtable(L, 0, 1);
	lua_pushcfunction(L, collect);
	lua_setfield(L, -2, "__gc");
	metatable_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	luaL_newlib(L, mylib);
	return 1;
}
