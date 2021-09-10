#include "httpng_private.h"

#include <fcntl.h>
#include <float.h>

#include <lauxlib.h>
#include <module.h>

#include <openssl/err.h>
#include "openssl_utils.h"

#ifndef lengthof
#define lengthof(array) (sizeof(array) / sizeof((array)[0]))
#endif

#ifdef TCP_FASTOPEN
#define H2O_DEFAULT_LENGTH_TCP_FASTOPEN_QUEUE 4096
#else
#define H2O_DEFAULT_LENGTH_TCP_FASTOPEN_QUEUE 0
#endif /* TCP_FASTOPEN */

#define QUEUE_TO_TX_ITEMS (1 << 12) /* Must be power of 2. */
#define QUEUE_FROM_TX_ITEMS QUEUE_TO_TX_ITEMS /* Must be power of 2. */

/* libh2o uses these magic values w/o defines. */
#define H2O_DEFAULT_PORT_FOR_PROTOCOL_USED 65535
#define H2O_CONTENT_LENGTH_UNSPECIFIED SIZE_MAX

#define DEFAULT_MIN_TLS_PROTO_VERSION_NUM TLS1_2_VERSION
#define DEFAULT_OPENSSL_SECURITY_LEVEL 1

#define STR_PORT_LENGTH 8

typedef struct listener_cfg {
	SSL_CTX *ssl_ctx;
	int fd;
	bool is_opened;
} listener_cfg_t;

typedef int lua_handler_func_t(lua_h2o_handler_t *, h2o_req_t *);

conf_t conf = {
	.tfo_queues = H2O_DEFAULT_LENGTH_TCP_FASTOPEN_QUEUE,
	.max_shuttles_per_thread = DEFAULT_max_shuttles_per_thread,
	.openssl_security_level = DEFAULT_OPENSSL_SECURITY_LEVEL,
	.min_tls_proto_version = DEFAULT_MIN_TLS_PROTO_VERSION_NUM,
};

__thread thread_ctx_t *curr_thread_ctx;

static uint32_t box_null_cdata_type;

static const char shuttle_field_name[] = "_shuttle";
static const char headers_not_table_msg[] = "headers is not a table";
static const char headers_invalid_msg[] = "headers are invalid";

static bool fill_http_headers(lua_State *L, lua_handler_state_t *state,
	int param_lua_idx);

/* Launched in TX thread. */
static inline void
my_xtm_delete_queue_from_tx(thread_ctx_t *thread_ctx)
{
	xtm_queue_delete(thread_ctx->queue_from_tx,
		XTM_QUEUE_MUST_CLOSE_PRODUCER_READFD | (
		thread_ctx->queue_from_tx_fd_consumed ? 0 :
		XTM_QUEUE_MUST_CLOSE_CONSUMER_READFD));
}

/* Launched in TX thread. */
static inline bool
is_cdata(lua_State *L, int idx)
{
	return luaL_iscdata(L, idx);
}

/* Launched in TX thread. */
static inline bool
is_box_null(lua_State *L, int idx)
{
	if (!is_cdata(L, idx))
	       return false;
	uint32_t cdata_type;
	void * *const ptr = (void **)luaL_checkcdata(L, idx, &cdata_type);
	return box_null_cdata_type == cdata_type && *ptr == NULL;
}

/* Launched in TX thread. */
static inline bool
is_nil_or_null(lua_State *L, int idx)
{
	return lua_isnil(L, idx) || is_box_null(L, idx);
}

/* Launched in TX thread. */
static inline bool
lua_isstring_strict(lua_State *L, int idx)
{
	return lua_type(L, idx) == LUA_TSTRING;
}

/* Launched in TX thread. */
static inline void
invoke_all_in_tx(struct xtm_queue *queue)
{
	/* FIXME: Maybe we should log consume error (should never happen?) */
	(void)xtm_queue_consume(xtm_queue_consumer_fd(queue));

	(void)xtm_queue_invoke_funs_all(queue);
	if (xtm_queue_get_reset_was_full(queue))
		while (xtm_queue_notify_producer(queue) != 0) {
			/* FIXME: Maybe we should log error
			 * (should not happen normally)? */
			fiber_sleep(0.001);
		}
}

/* Launched in TX thread.
 * FIXME: Use lua_tointegerx() when we would no longer care about
 * older Tarantool versions. */
static inline lua_Integer
my_lua_tointegerx(lua_State *L, int idx, int *ok)
{
	return (*ok = lua_isnumber(L, idx)) ? lua_tointeger(L, idx) : 0;
}

/* Launched in TX thread. */
static inline void
reliably_notify_xtm_consumer_from_tx(struct xtm_queue *queue)
{
	while (xtm_queue_notify_consumer(queue) != 0) {
		/* Actually notification should never fail, but... */
		assert(false);
		fiber_sleep(0.001);
	}
}

/* Launched in TX thread. */
static inline void
wake_call_from_tx_waiter(thread_ctx_t *thread_ctx)
{
	waiter_t *const waiter = thread_ctx->call_from_tx_waiter;
	if (waiter == NULL)
		return;

	thread_ctx->call_from_tx_waiter = waiter->next;
	fiber_wakeup(waiter->fiber);
}

/* Launched in TX thread.
 * N. b.: xtm queue is single-producer, there should be no pushes to the same
 * queue from HTTP(S) server threads until we are done. */
static void
call_from_tx(struct xtm_queue *queue, void *func, void *param,
	thread_ctx_t *thread_ctx)
{
	if (xtm_queue_push_fun(queue, (void (*)(void *))func, param,
	    XTM_QUEUE_PRODUCER_NEEDS_NOTIFICATIONS) == 0) {
		reliably_notify_xtm_consumer_from_tx(queue);
		return;
	}

	do {
		if (thread_ctx->push_from_tx_is_sleeping) {
			waiter_t waiter =
				{ .next = NULL, .fiber = fiber_self() };
			waiter_t *last_waiter =
				thread_ctx->call_from_tx_waiter;
			if (last_waiter == NULL)
				thread_ctx->call_from_tx_waiter = &waiter;
			else {
				/* FIXME: It may be more efficient to use
				 * double-linked list if we expect a lot of
				 * competing fibers. */
				while (last_waiter->next != NULL)
					last_waiter = last_waiter->next;
				last_waiter->next = &waiter;
			}
			fiber_yield();
		} else {
			const int fd = xtm_queue_producer_fd(queue);
			thread_ctx->push_from_tx_is_sleeping = true;
			const int res = coio_wait(fd, COIO_READ, DBL_MAX);
			thread_ctx->push_from_tx_is_sleeping = false;
			if (res & COIO_READ)
				/* FIXME: Maybe we should log consume error
				 * (should never happen?) */
				(void)xtm_queue_consume(fd);
		}
	} while (xtm_queue_push_fun(queue, (void (*)(void *))func, param,
		XTM_QUEUE_PRODUCER_NEEDS_NOTIFICATIONS) != 0);
	reliably_notify_xtm_consumer_from_tx(queue);
	wake_call_from_tx_waiter(thread_ctx);
}

/* Launched in TX thread. */
static inline void
call_in_http_thr_with_shuttle(shuttle_func_t *func, shuttle_t *shuttle)
{
	call_from_tx(shuttle->thread_ctx->queue_from_tx, func, shuttle,
		shuttle->thread_ctx);
}

/* Launched in HTTP server thread. */
static void
free_shuttle_internal(shuttle_t *shuttle)
{
	assert(shuttle->disposed);
	free_shuttle(shuttle);
}

/* Launched in TX thread. */
static void
free_shuttle_from_tx_in_http_thr(shuttle_t *shuttle)
{
	call_in_http_thr_with_shuttle(free_shuttle_internal, shuttle);
}

/* Launched in TX thread.
 * It can queue request to HTTP server thread or free everything itself. */
void
free_shuttle_from_tx(shuttle_t *shuttle)
{
	free_shuttle_from_tx_in_http_thr(shuttle);
}

/* Launched in TX thread. */
static inline void
free_lua_shuttle_from_tx_in_http_thr(shuttle_t *shuttle)
{
	free_shuttle_from_tx_in_http_thr(shuttle);
}

/* Launched in TX thread. */
static inline void
free_cancelled_lua_not_ws_shuttle_from_tx(shuttle_t *shuttle)
{
	free_shuttle_from_tx_in_http_thr(shuttle);
}

/* Launched in TX thread. */
static inline void
free_cancelled_lua_shuttle_from_tx(lua_handler_state_t *state)
{
	shuttle_t *const shuttle = get_shuttle(state);
	free_cancelled_lua_not_ws_shuttle_from_tx(shuttle);
}

/* Launched in TX thread. */
static inline void
wakeup_waiter(lua_handler_state_t *state)
{
	assert(state->waiter != NULL);
	struct fiber *const fiber = state->waiter->fiber;
	assert(fiber != NULL);
	state->waiter = state->waiter->next;
	fiber_wakeup(fiber);
}

/* Launched in TX thread. */
void
cancel_processing_lua_req_in_tx(shuttle_t *shuttle)
{
	lua_handler_state_t *const state =
		(lua_handler_state_t *)&shuttle->payload;

	/* We do not use fiber_cancel() because it causes exception
	 * in Lua code so Lua handler have to use pcall() and even
	 * that is not 100% guarantee because such exception
	 * can theoretically happen before pcall().
	 * Also we have to unref Lua state. */
	if (state->waiter != NULL) {
		assert(!state->fiber_done);
		state->cancelled = true;
		wakeup_waiter(state);
	} else if (state->fiber_done) {
		assert(!state->cancelled);
		free_cancelled_lua_not_ws_shuttle_from_tx(shuttle);
	} else
		state->cancelled = true;
		/* Fiber would clean up because we have set cancelled=true. */
}

/* Launched in TX thread. */
void
continue_processing_lua_req_in_tx(lua_handler_state_t *state)
{
	assert(state->fiber != NULL);
	assert(!state->fiber_done);
	wakeup_waiter(state);
}

/* Launched in TX thread. */
static inline void
call_in_http_thr_postprocess_lua_req_first(shuttle_t *shuttle)
{
	call_in_http_thr_with_shuttle(postprocess_lua_req_first, shuttle);
}

/* Launched in TX thread. */
static inline void
call_in_http_thr_postprocess_lua_req_others(shuttle_t *shuttle)
{
	call_in_http_thr_with_shuttle(postprocess_lua_req_others, shuttle);
}

/* Launched in TX thread. */
static inline void
add_http_header_to_lua_response(lua_first_response_only_t *state,
	const char *key, size_t key_len,
	const char *value, size_t value_len)
{
	if (state->num_headers >= conf.max_headers_lua)
		/* FIXME: Misconfiguration, should we log something? */
		return;

	state->headers[state->num_headers++] = (http_header_entry_t)
		{key, value, (unsigned)key_len, (unsigned)value_len};
}

/* Launched in TX thread.
 * Makes sure earlier queued sends to HTTP server thread are done
 * OR cancellation request is received. */
static void
take_shuttle_ownership_lua(lua_handler_state_t *state)
{
	if (state->cancelled || state->waiter == NULL)
		return;

	/* Other fiber(s) are already waiting for shuttle return or taking
	 * ownership, add ourself into tail of waiting list. */
	waiter_t waiter = { .next = NULL, .fiber = fiber_self() };
	waiter_t *last_waiter = state->waiter;
	/* FIXME: It may be more efficient to use double-linked list
	 * if we expect a lot of competing fibers. */
	while (last_waiter->next != NULL)
		last_waiter = last_waiter->next;
	last_waiter->next = &waiter;
	fiber_yield();
}

/* Launched in TX thread.
 * Caller must call take_shuttle_ownership_lua() before filling in shuttle
 * and calling us.
 * N. b.: We may have been awoken by cancellation request. */
static inline void
wait_for_lua_shuttle_return_internal(lua_handler_state_t *state)
{
	/* Add us into head of waiting list. */
	waiter_t waiter = { .next = state->waiter, .fiber = fiber_self() };
	state->waiter = &waiter;
	fiber_yield();
	if (state->waiter != NULL)
		wakeup_waiter(state);
}

/* Launched in TX thread.
 * Caller must call take_shuttle_ownership_lua() before filling in shuttle
 * and calling us.
 * N. b.: We may have been awoken by cancellation request. */
static inline void
wait_for_lua_shuttle_return(lua_handler_state_t *state)
{
	if (state->cancelled)
		return;

	wait_for_lua_shuttle_return_internal(state);
}

/* Launched in TX thread. */
static inline int
get_default_http_code(lua_handler_state_t *state)
{
	assert(!state->sent_something);
	return 200; /* FIXME: Could differ depending on HTTP request type. */
}

/* Launched in TX thread. */
static int
perform_write(lua_State *L)
{
	/* Lua parameters: self, payload. */
	enum {
		LUA_STACK_IDX_SELF = 1,
		LUA_STACK_IDX_PAYLOAD = 2,
		LUA_STACK_REQUIRED_PARAMS_COUNT = LUA_STACK_IDX_PAYLOAD,
	};
	const unsigned num_params = lua_gettop(L);
	if (num_params < LUA_STACK_REQUIRED_PARAMS_COUNT)
		return luaL_error(L, "Not enough parameters");

	lua_getfield(L, LUA_STACK_IDX_SELF, shuttle_field_name);
	if (!lua_islightuserdata(L, -1))
		return luaL_error(L, "shuttle is invalid");
	shuttle_t *const shuttle = (shuttle_t *)lua_touserdata(L, -1);

	lua_handler_state_t *const state =
		(lua_handler_state_t *)&shuttle->payload;
	take_shuttle_ownership_lua(state);
	if (state->cancelled)
		return 0;

	size_t payload_len;
	state->un.resp.any.payload =
		lua_tolstring(L, LUA_STACK_IDX_PAYLOAD, &payload_len);
	state->un.resp.any.payload_len = payload_len;

	bool is_last;
		is_last = false;

	state->un.resp.any.is_last_send = is_last;
	if (!state->sent_something) {
		state->un.resp.first.http_code = get_default_http_code(state);

		lua_getfield(L, LUA_STACK_IDX_SELF, "headers");
		const unsigned headers_lua_index = num_params + 1 + 1;
		if (!lua_isnil(L, headers_lua_index) &&
		    !lua_istable(L, headers_lua_index))
			return luaL_error(L, headers_not_table_msg);
		if (!fill_http_headers(L, state, headers_lua_index))
			return luaL_error(L, headers_invalid_msg);

		state->sent_something = true;
		call_in_http_thr_postprocess_lua_req_first(shuttle);
	} else
		call_in_http_thr_postprocess_lua_req_others(shuttle);
	wait_for_lua_shuttle_return(state);

	return 0;
}

/* Launched in TX thread. */
static bool
fill_http_headers(lua_State *L, lua_handler_state_t *state, int param_lua_idx)
{
	state->un.resp.first.content_length = H2O_CONTENT_LENGTH_UNSPECIFIED;
	if (lua_isnil(L, param_lua_idx))
		return true;

	lua_pushnil(L); /* Start of table. */
	while (lua_next(L, param_lua_idx)) {
		if (!lua_isstring_strict(L, -2)) {
			/* Can't use lua_tolstring(), it can convert
			 * e. g. number to string and lua_next()
			 * would assert.
			 * There is no point sending invalid headers anyway. */
			lua_pop(L, 2);
			return false;
		}

		size_t key_len;
		size_t value_len;
		const char *const key = lua_tolstring(L, -2, &key_len);
		const char *const value = lua_tolstring(L, -1, &value_len);

		static const char content_length_str[] = "content-length";
		char temp[32];
		if (key_len == sizeof(content_length_str) - 1 &&
		    !strncasecmp(key, content_length_str, key_len) &&
		    value_len < sizeof(temp)) {
			memcpy(temp, value, value_len);
			temp[value_len] = 0;
			errno = 0;
			const long long candidate = strtoll(temp, NULL, 10);
			if (errno)
				add_http_header_to_lua_response(
					&state->un.resp.first,
					key, key_len, value, value_len);
			else
				/* h2o would add this header
				 * and disable chunked. */
				state->un.resp.first.content_length =
					candidate;
		} else
			add_http_header_to_lua_response(&state->un.resp.first,
				key, key_len, value, value_len);

		/* Remove value, keep key for next iteration. */
		lua_pop(L, 1);
	}
	return true;
}

/* Launched in TX thread */
static inline void
set_no_payload(lua_handler_state_t *state)
{
	/* Pointer can't be garbage even with 0 len,
	 * w/o HTTPS libh2o passes it to low level OS functions
	 * which are not smart enough. */
	state->un.resp.any.payload = NULL;
	state->un.resp.any.payload_len = 0;
}

/* Launched in TX thread */
static int
perform_write_header(lua_State *L)
{
	/* Lua parameters: self, code, headers. */
	enum {
		LUA_STACK_IDX_SELF = 1,
		LUA_STACK_IDX_CODE = 2,
		LUA_STACK_REQUIRED_PARAMS_COUNT = LUA_STACK_IDX_CODE,
		LUA_STACK_IDX_HEADERS = 3,
	};
	const unsigned num_params = lua_gettop(L);
	if (num_params < LUA_STACK_REQUIRED_PARAMS_COUNT)
		return luaL_error(L, "Not enough parameters");

	lua_getfield(L, LUA_STACK_IDX_SELF, shuttle_field_name);

	if (!lua_islightuserdata(L, -1))
		return luaL_error(L, "shuttle is invalid");
	shuttle_t *const shuttle = (shuttle_t *)lua_touserdata(L, -1);

	bool is_last;
		is_last = false;

	lua_handler_state_t *const state =
		(lua_handler_state_t *)&shuttle->payload;
	take_shuttle_ownership_lua(state);
	if (state->sent_something)
		return luaL_error(L, "Handler has already written header");
	if (state->cancelled)
		return 0;

	int is_integer;
	state->un.resp.first.http_code =
		my_lua_tointegerx(L, LUA_STACK_IDX_CODE, &is_integer);
	if (!is_integer)
		return luaL_error(L, "HTTP code is not an integer");

	unsigned headers_lua_index;
	if (num_params >= LUA_STACK_IDX_HEADERS)
		headers_lua_index = LUA_STACK_IDX_HEADERS;
	else {
		lua_getfield(L, LUA_STACK_IDX_SELF, "headers");
		headers_lua_index = num_params + 1 + 1;
	}

	if (!lua_isnil(L, headers_lua_index) &&
	    !lua_istable(L, headers_lua_index))
		return luaL_error(L, headers_not_table_msg);
	if (!fill_http_headers(L, state, headers_lua_index))
		return luaL_error(L, headers_invalid_msg);

	set_no_payload(state);

	state->un.resp.any.is_last_send = is_last;
	state->sent_something = true;
	call_in_http_thr_postprocess_lua_req_first(shuttle);
	wait_for_lua_shuttle_return(state);

	return 0;
}

/* Launched in TX thread. */
static inline const char *
get_router_entry_id(const lua_handler_state_t *state)
{
	/* FIXME: Query router for actual entry like "/foo". */
	(void)state;
	return "<unknown>";
}

/* Launched in TX thread.
 * Returns !0 in case of error. */
static inline int
fill_received_headers_and_body(lua_State *L, shuttle_t *shuttle)
{
	lua_handler_state_t *const state =
		(lua_handler_state_t *)&shuttle->payload;
	assert(!state->sent_something);
	unsigned current_offset = state->un.req.router_data_len +
		state->un.req.path_len + state->un.req.authority_len;
	const received_http_header_handle_t *const handles =
		(received_http_header_handle_t *)&state->un.req.buffer[
		current_offset];
	const unsigned num_headers = state->un.req.num_headers;
	lua_createtable(L, 0, num_headers);
	current_offset += num_headers * sizeof(received_http_header_handle_t);
	unsigned header_idx;
	for (header_idx = 0; header_idx < num_headers; ++header_idx) {
		const received_http_header_handle_t *const handle =
			&handles[header_idx];
		lua_pushlstring(L, &state->un.req.buffer[current_offset +
			handle->name_size + 1], handle->value_size);

		/* N. b.: it must be NULL-terminated. */
		lua_setfield(L, -2, &state->un.req.buffer[current_offset]);

		current_offset += handle->name_size + 1 + handle->value_size;
	}
	lua_setfield(L, -2, "headers");
	{
		lua_pushlstring(L, &state->un.req.buffer[current_offset],
			state->un.req.body_len);
		lua_setfield(L, -2, "body");
		return 0;
	}
}

/* Launched in TX thread. */
static void
close_lua_req_internal(shuttle_t *shuttle)
{
	lua_handler_state_t *const state =
		(lua_handler_state_t *)&shuttle->payload;
	take_shuttle_ownership_lua(state);
	if (state->cancelled)
		return;

	set_no_payload(state);
	state->un.resp.any.is_last_send = true;
	if (state->sent_something)
		call_in_http_thr_postprocess_lua_req_others(shuttle);
	else {
		state->un.resp.first.http_code = get_default_http_code(state);
		state->sent_something = true;
		state->un.resp.first.content_length =
			H2O_CONTENT_LENGTH_UNSPECIFIED;
		call_in_http_thr_postprocess_lua_req_first(shuttle);
	}
	wait_for_lua_shuttle_return(state);
}

/* Launched in TX thread. */
static int
perform_close(lua_State *L)
{
	/* Lua parameters: self. */
	enum {
		LUA_STACK_IDX_SELF = 1,
		LUA_STACK_REQUIRED_PARAMS_COUNT = LUA_STACK_IDX_SELF,
	};
	const unsigned num_params = lua_gettop(L);
	if (num_params < LUA_STACK_REQUIRED_PARAMS_COUNT)
		return luaL_error(L, "Not enough parameters");

	lua_getfield(L, LUA_STACK_IDX_SELF, shuttle_field_name);
	if (!lua_islightuserdata(L, -1))
		return luaL_error(L, "shuttle is invalid");
	shuttle_t *const shuttle = (shuttle_t *)lua_touserdata(L, -1);
	close_lua_req_internal(shuttle);
	return 0;
}

/* Launched in TX thread. */
static void
finish_handler_processing(shuttle_func_t *func, shuttle_t *shuttle)
{
	call_in_http_thr_with_shuttle(func, shuttle);
	lua_handler_state_t *const state =
		(lua_handler_state_t *)&shuttle->payload;
	wait_for_lua_shuttle_return(state);
	if (state->cancelled)
		/* There would be no more calls from HTTP server thread,
		 * must clean up. */
		free_lua_shuttle_from_tx_in_http_thr(shuttle);
	else
		state->fiber_done = true;
		/* cancel_processing_lua_req_in_tx() is not yet called,
		 * it would clean up because we have set fiber_done=true. */
}

/* Launched in TX thread. */
static inline void
process_handler_failure_not_ws(shuttle_t *shuttle)
{
	lua_handler_state_t *const state =
		(lua_handler_state_t *)&shuttle->payload;
	take_shuttle_ownership_lua(state);
	if (state->cancelled) {
		/* There would be no more calls from HTTP server thread,
		 * must clean up. */
		free_cancelled_lua_not_ws_shuttle_from_tx(shuttle);
		return;
	}

	state->un.resp.any.is_last_send = true;
	shuttle_func_t *func;
	if (state->sent_something) {
		/* Do not add anything to user output to prevent
		 * corrupt HTML etc. */
		set_no_payload(state);
		func = &postprocess_lua_req_others;
	} else {
		static const char key[] = "content-type";
		static const char value[] = "text/plain; charset=utf-8";
		add_http_header_to_lua_response(&state->un.resp.first, key,
			sizeof(key) - 1, value, sizeof(value) - 1);
		static const char error_str[] = "Path handler execution error";
		state->un.resp.first.http_code = 500;
		state->un.resp.any.payload = error_str;
		state->un.resp.any.payload_len = sizeof(error_str) - 1;
		state->un.resp.first.content_length = sizeof(error_str) - 1;
		state->sent_something = true;
		func = &postprocess_lua_req_first;
	}
	finish_handler_processing(func, shuttle);
}

/* Launched in TX thread. */
static inline void
process_handler_success_not_ws_with_send(lua_State *L, shuttle_t *shuttle)
{
	lua_handler_state_t *const state =
		(lua_handler_state_t *)&shuttle->payload;
	take_shuttle_ownership_lua(state);
	if (state->cancelled) {
		/* There would be no more calls from HTTP server
		 * thread, must clean up. */
		free_cancelled_lua_not_ws_shuttle_from_tx(shuttle);
		return;
	}
	const bool old_sent_something = state->sent_something;
	if (!old_sent_something) {
		lua_getfield(L, -1, "status");
		if (lua_isnil(L, -1))
			state->un.resp.first.http_code =
				get_default_http_code(state);
		else {
			int is_integer;
			state->un.resp.first.http_code =
				my_lua_tointegerx(L, -1, &is_integer);
			if (!is_integer)
				state->un.resp.first.http_code =
					get_default_http_code(state);
		}
		lua_getfield(L, -2, "headers");
		const int idx = lua_gettop(L);
		if (lua_istable(L, idx))
			fill_http_headers(L, state, idx);
		lua_pop(L, 2); /* headers, status. */
		state->sent_something = true;
	}

	lua_getfield(L, -1, "body");
	if (!lua_isnil(L, -1)) {
		size_t payload_len;
		state->un.resp.any.payload =
			lua_tolstring(L, -1, &payload_len);
		state->un.resp.any.payload_len = payload_len;
	} else
		set_no_payload(state);

	state->un.resp.any.is_last_send = true;

	if (old_sent_something)
		finish_handler_processing(postprocess_lua_req_others, shuttle);
	else {
		state->un.resp.first.content_length =
			state->un.resp.any.payload_len;
		finish_handler_processing(postprocess_lua_req_first, shuttle);
	}
}

/* Launched in TX thread. */
static inline void
push_path(lua_State *L, lua_handler_state_t *state)
{
	const size_t len = (state->un.req.query_at == LUA_QUERY_NONE) ?
		state->un.req.path_len : state->un.req.query_at;
	lua_pushlstring(L,
		&state->un.req.buffer[state->un.req.router_data_len], len);
	lua_setfield(L, -2, "path");
}

/* Launched in TX thread. */
static inline void
push_query(lua_State *L, lua_handler_state_t *state)
{
	if (state->un.req.query_at == LUA_QUERY_NONE)
		return;

	int64_t query_at = state->un.req.query_at;
	const unsigned len = state->un.req.path_len;

	if ((uint64_t)query_at > (uint64_t)len) {
		assert(false);
		return;
	}

	const char *const path = state->un.req.buffer;
	query_at += 1;

	/* N.b.: query_at is 1-based; we also skip '?'. */
	lua_pushlstring(L, path + query_at, len - query_at);
	lua_setfield(L, -2, "query");
}

/* Launched in TX thread. */
static inline void
process_internal_error(shuttle_t *shuttle)
{
	lua_handler_state_t *const state =
		(lua_handler_state_t *)&shuttle->payload;

	assert(!state->sent_something);
	state->un.resp.first.num_headers = 0;
	state->un.resp.any.is_last_send = true;
	static const char key[] = "content-type";
	static const char value[] = "text/plain; charset=utf-8";
	add_http_header_to_lua_response(&state->un.resp.first, key,
		sizeof(key) - 1, value, sizeof(value) - 1);
	static const char error_str[] = "Internal error";
	state->un.resp.first.http_code = 500;
	state->un.resp.any.payload = error_str;
	state->un.resp.any.payload_len = sizeof(error_str) - 1;
	state->un.resp.first.content_length = sizeof(error_str) - 1;
	state->sent_something = true;

	finish_handler_processing(&postprocess_lua_req_first, shuttle);
}

/* Launched in TX thread. */
static inline void
process_handler_success_not_ws_without_send(shuttle_t *shuttle)
{
	lua_handler_state_t *const state =
		(lua_handler_state_t *)&shuttle->payload;
	if (state->un.resp.any.is_last_send) {
	Done:
		state->fiber_done = true;
		/* cancel_processing_lua_req_in_tx() is not yet called,
		 * it would clean up because we have set fiber_done=true. */
		return;
	}

	close_lua_req_internal(shuttle);
	if (!state->cancelled)
		goto Done;

	free_cancelled_lua_not_ws_shuttle_from_tx(shuttle);
}

/* Launched in TX thread. */
static int
lua_fiber_func(va_list ap)
{
	shuttle_t *const shuttle = va_arg(ap, shuttle_t *);
	lua_State *const L = va_arg(ap, lua_State *);

	lua_handler_state_t *const state =
		(lua_handler_state_t *)&shuttle->payload;

	/* User handler function, written in Lua. */
	lua_rawgeti(L, LUA_REGISTRYINDEX, conf.lua_handler_ref);

	/* First param for Lua handler - req. */
	lua_createtable(L, 0, 15);
	lua_pushinteger(L, state->un.req.version_major);
	lua_setfield(L, -2, "version_major");
	lua_pushinteger(L, state->un.req.version_minor);
	lua_setfield(L, -2, "version_minor");
	push_path(L, state);
	lua_pushlstring(L,
		&state->un.req.buffer[state->un.req.router_data_len +
			state->un.req.path_len], state->un.req.authority_len);
	lua_setfield(L, -2, "host");
	push_query(L, state);
	lua_pushlstring(L, state->un.req.method, state->un.req.method_len);
	lua_setfield(L, -2, "method");
	lua_pushlightuserdata(L, shuttle);
	lua_setfield(L, -2, shuttle_field_name);

	const int lua_state_ref = state->lua_state_ref;
	if (fill_received_headers_and_body(L, shuttle)) {
		process_internal_error(shuttle);
		goto Done;
	}

	/* We have finished parsing request, now can write to response
	 * (it is union). */
	state->un.resp.first.num_headers = 0;
	state->un.resp.any.is_last_send = false;

	/* Second param for Lua handler - io. */
	lua_createtable(L, 0, 5
			);
	lua_pushcfunction(L, perform_write_header);
	lua_setfield(L, -2, "write_header");
	lua_pushcfunction(L, perform_write);
	lua_setfield(L, -2, "write");
	lua_pushcfunction(L, perform_close);
	lua_setfield(L, -2, "close");
	lua_pushlightuserdata(L, shuttle);
	lua_setfield(L, -2, shuttle_field_name);
	lua_createtable(L, 0, 2);
	lua_setfield(L, -2, "headers");

	if (lua_pcall(L, 2, 1, 0) != LUA_OK) {
		/* FIXME: Should probably log this instead(?) */
		fprintf(stderr, "User handler for \"%s\" failed with error "
			"\"%s\"\n", get_router_entry_id(state),
			lua_tostring(L, -1));

		if (state->cancelled)
			free_cancelled_lua_shuttle_from_tx(state);
		else
			process_handler_failure_not_ws(shuttle);
	} else if (state->cancelled)
		free_cancelled_lua_shuttle_from_tx(state);
	else if (lua_isnil(L, -1))
		process_handler_success_not_ws_without_send(shuttle);
	else
		process_handler_success_not_ws_with_send(L, shuttle);

Done:
	luaL_unref(luaT_state(), LUA_REGISTRYINDEX, lua_state_ref);

	return 0;
}

/* Launched in TX thread. */
void
process_lua_req_in_tx(shuttle_t *shuttle)
{
	lua_handler_state_t *const state =
		(lua_handler_state_t *)&shuttle->payload;

#define RETURN_WITH_ERROR(err) \
	do { \
		luaL_unref(L, LUA_REGISTRYINDEX, state->lua_state_ref); \
		static const char key[] = "content-type"; \
		static const char value[] = "text/plain; charset=utf-8"; \
		state->un.resp.first.num_headers = 0; \
		add_http_header_to_lua_response(&state->un.resp.first, \
			key, sizeof(key) - 1, value, sizeof(value) - 1); \
		static const char error_str[] = err; \
		state->un.resp.any.is_last_send = true; \
		state->un.resp.first.http_code = 500; \
		state->un.resp.any.payload = error_str; \
		state->un.resp.any.payload_len = sizeof(error_str) - 1; \
		state->sent_something = true; \
		state->un.resp.first.content_length = sizeof(error_str) - 1; \
		call_in_http_thr_postprocess_lua_req_first(shuttle); \
		return; \
	} while (0)

	struct lua_State *const L = luaT_state();
	struct lua_State *const new_L = lua_newthread(L);
	state->lua_state_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	if ((state->fiber = fiber_new("HTTP Lua fiber", &lua_fiber_func))
	    == NULL)
		RETURN_WITH_ERROR("Failed to create fiber");
	state->fiber_done = false;
	fiber_start(state->fiber, shuttle, new_L);
}
#undef RETURN_WITH_ERROR

/* Launched in TX thread. */
static h2o_pathconf_t *
register_complex_handler_part_two(h2o_hostconf_t *hostconf,
	lua_handler_func_t *c_handler,
	void *c_handler_param, int router_ref)
{
	/* These functions never return NULL, dying instead */
	h2o_pathconf_t *pathconf =
		h2o_config_register_path(hostconf, "/", 0);
	lua_h2o_handler_t *handler = (lua_h2o_handler_t *)
		h2o_create_handler(pathconf, sizeof(*handler));
	handler->super.on_req =
		(int (*)(h2o_handler_t *, h2o_req_t *))c_handler;
	conf.lua_handler = handler;
	return pathconf;
}

/* Launched in TX thread. */
static h2o_pathconf_t *
register_lua_handler_part_two(h2o_hostconf_t *hostconf)
{
	return register_complex_handler_part_two(hostconf, lua_req_handler,
		NULL, LUA_REFNIL);
}

/* Launched in TX thread. */
static void
register_lua_handler(int lua_handler_ref)
{
	conf.lua_handler_ref = lua_handler_ref;
	unsigned thread_idx;
	for (thread_idx = 0; thread_idx < MAX_threads; ++thread_idx)
		register_lua_handler_part_two(conf.thread_ctxs[thread_idx]
			.hostconf);
}

/* Launched in TX thread. */
static inline void
set_cloexec(int fd)
{
	/* For performance reasons do not check result in production builds
	 * (should not fail anyway).
	 * TODO: Remove this call completely? Do we plan to create
	 * child processes ? */
	int result = fcntl(fd, F_SETFD, FD_CLOEXEC);
	assert(result != -1);
	(void)result; /* To build w/disabled assert(). */
}

/* Launched in TX thread. */
static void
register_listener_cfgs_socket(int fd, SSL_CTX *ssl_ctx, unsigned listener_idx)
{
	assert(listener_idx < conf.num_listeners);
	listener_cfg_t *const listener_cfg = &conf.listener_cfgs[listener_idx];
	assert(!listener_cfg->is_opened);
	listener_cfg->fd = fd;
	listener_cfg->ssl_ctx = ssl_ctx;
	listener_cfg->is_opened = true;
}

/* Launched in TX thread. */
static void
close_listener_cfgs_sockets(void)
{
	unsigned listener_idx;
	for (listener_idx = 0; listener_idx < conf.num_listeners;
	    ++listener_idx) {
		listener_cfg_t *const listener_cfg =
		    &conf.listener_cfgs[listener_idx];
		if (listener_cfg->is_opened) {
			close(listener_cfg->fd);
			listener_cfg->is_opened = false;
		}
		if (listener_cfg->ssl_ctx != NULL) {
			SSL_CTX_free(listener_cfg->ssl_ctx);
			listener_cfg->ssl_ctx = NULL;
		}
	}
}

/* Launched in TX thread. */
static bool
prepare_listening_sockets(thread_ctx_t *thread_ctx)
{
	unsigned listener_idx;
	for (listener_idx = 0; listener_idx < conf.num_listeners;
	    ++listener_idx) {
		listener_ctx_t *const listener_ctx =
		    &thread_ctx->listener_ctxs[listener_idx];
		const listener_cfg_t *const listener_cfg =
		    &conf.listener_cfgs[listener_idx];

		assert(listener_cfg->is_opened);
		memset(listener_ctx, 0, sizeof(*listener_ctx));
		listener_ctx->accept_ctx.ssl_ctx = listener_cfg->ssl_ctx;
		listener_ctx->accept_ctx.ctx = &thread_ctx->ctx;
		listener_ctx->accept_ctx.hosts = thread_ctx->globalconf.hosts;
		listener_ctx->sock = NULL;

		if (thread_ctx->idx) {
			if ((listener_ctx->fd = dup(listener_cfg->fd)) < 0)
				return false;
		} else
			listener_ctx->fd = listener_cfg->fd;
		set_cloexec(listener_ctx->fd);
		thread_ctx->listeners_created++;
	}
	return true;
}

/* Can be launched in TX thread or HTTP server thread. */
void
close_listening_sockets(thread_ctx_t *thread_ctx)
{
	listener_ctx_t *const listener_ctxs = thread_ctx->listener_ctxs;

	unsigned listener_idx;
	/* FIXME: What if listeners_created==0 for thread #0?
	 * It looks like we would not close fd. */
	for (listener_idx = 0; listener_idx < thread_ctx->listeners_created;
	    ++listener_idx) {
		listener_ctx_t *const listener_ctx =
		    &listener_ctxs[listener_idx];
		close(listener_ctx->fd);
	}
	thread_ctx->listeners_created = 0;
}

/* Launched in TX thread. */
static void
deinit_listener_cfgs(void)
{
	unsigned listener_idx;
	for (listener_idx = 0; listener_idx < conf.num_listeners;
	    ++listener_idx) {
		listener_cfg_t *const listener_cfg =
		    &conf.listener_cfgs[listener_idx];
		listener_cfg->is_opened = false;
		if (listener_cfg->ssl_ctx != NULL) {
			SSL_CTX_free(listener_cfg->ssl_ctx);
			listener_cfg->ssl_ctx = NULL;
		}
	}
}

/* Launched in TX thread. */
static int
ip_version(const char *src)
{
	char buf[sizeof(struct in6_addr)];
	if (inet_pton(AF_INET, src, buf))
		return AF_INET;
	if (inet_pton(AF_INET6, src, buf))
		return AF_INET6;
	return -1;
}

/* Launched in TX thread.
 * Returns file descriptor or -1 on error. */
static int
open_listener(const char *addr_str, uint16_t port, const char **lerr)
{
	struct addrinfo hints, *res;
	char port_str[STR_PORT_LENGTH];
	snprintf(port_str, sizeof(port_str), "%d", port);

	memset(&hints, 0, sizeof(hints));

	int ai_family = ip_version(addr_str);
	if (ai_family < 0) {
		*lerr = "Can't parse IP address";
		goto ip_detection_fail;
	}

	hints.ai_family = ai_family;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST | AI_NUMERICSERV;

	int ret = getaddrinfo(addr_str, port_str, &hints, &res);
	if (ret || res->ai_family != ai_family) {
		*lerr = "getaddrinfo can't find appropriate ip and port";
		goto getaddrinfo_fail;
	}

	int flags = SOCK_STREAM;
#ifdef SOCK_CLOEXEC
	flags |= SOCK_CLOEXEC;
#endif /* SOCK_CLOEXEC */
	int fd;
	if ((fd = socket(res->ai_family, flags, 0)) < 0) {
		*lerr = "create socket failed";
		goto socket_create_fail;
	}
#ifndef SOCK_CLOEXEC
	if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
		*lerr = "setting FD_CLOEXEC failed";
		goto fdcloexec_set_fail;
	}
#endif /* SOCK_CLOEXEC */

	int reuseaddr_flag = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuseaddr_flag,
	    sizeof(reuseaddr_flag)) != 0) {
		*lerr = "setsockopt SO_REUSEADDR failed";
		goto so_reuseaddr_set_fail;
	}

	int ipv6_flag = 1;
	if (ai_family == AF_INET6 && setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY,
	    &ipv6_flag, sizeof(ipv6_flag)) != 0) {
		*lerr = "setsockopt IPV6_V6ONLY failed";
		goto ipv6_only_set_fail;
	}

	if (bind(fd, res->ai_addr, res->ai_addrlen) != 0) {
		*lerr = "bind error";
		goto bind_fail;
	}

	if (listen(fd, SOMAXCONN) != 0) {
		*lerr = "listen error";
		goto listen_fail;
	}

#ifdef TCP_DEFER_ACCEPT
	{
		/* We are only interested in connections
		 * when actual data is received. */
		int flag = 1;
		if (setsockopt(fd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &flag,
		    sizeof(flag)) != 0)
			fprintf(stderr, "setting TCP_DEFER_ACCEPT failed\n");
	}
#endif /* TCP_DEFER_ACCEPT */

	if (conf.tfo_queues > 0) {
		/* TCP extension to do not wait for SYN/ACK for "known"
		 * clients. */
#ifdef TCP_FASTOPEN
		int tfo_queues;
#ifdef __APPLE__
		/* In OS X, the option value for TCP_FASTOPEN must be 1
		 * if is's enabled. */
		tfo_queues = 1;
#else /* __APPLE__ */
		tfo_queues = conf.tfo_queues;
#endif /* __APPLE__ */
		if (setsockopt(fd, IPPROTO_TCP, TCP_FASTOPEN,
		    (const void *)&tfo_queues, sizeof(tfo_queues)) != 0)
			fprintf(stderr, "setting TCP TFO feature failed\n");
#else /* TCP_FASTOPEN */
		assert(!".tfo_queues not zero on platform w/o TCP_FASTOPEN");
#endif /* TCP_FASTOPEN */
	}

	return fd;

listen_fail:
bind_fail:
ipv6_only_set_fail:
so_reuseaddr_set_fail:
#ifndef SOCK_CLOEXEC
fdcloexec_set_fail:
#endif /* SOCK_CLOEXEC */
	close(fd);
socket_create_fail:
getaddrinfo_fail:
ip_detection_fail:
	assert(*lerr != NULL);
	return -1;
}

/* Launched in TX thread. */
static int
load_default_listen_params(const char **lerr)
{
	conf.num_listeners = 2;
	static const char hardcoded_certificate_file[] = "examples/cert.pem";
	static const char hardcoded_certificate_key_file[] = "examples/key.pem";
	if ((conf.listener_cfgs =
	    (typeof(conf.listener_cfgs))calloc(conf.num_listeners,
	    sizeof(*conf.listener_cfgs))) == NULL) {
		*lerr = "allocation memory for listener_cfgs failed";
		goto Error;
	}

	static const uint16_t port = 3300;
	{
		const char *const addr = "0.0.0.0";

		const int fd = open_listener(addr, port, lerr);
		if (fd < 0)
			goto Error;
		SSL_CTX *const ssl_ctx =
			make_ssl_ctx(hardcoded_certificate_file,
				hardcoded_certificate_key_file,
				conf.openssl_security_level,
				conf.min_tls_proto_version, lerr);
		if (ssl_ctx == NULL) {
			close(fd);
			goto Error;
		}
		register_listener_cfgs_socket(fd, ssl_ctx, 0);
	}

	{
		const char *const addr = "::";

		const int fd = open_listener(addr, port, lerr);
		if (fd < 0)
			goto Error;
		SSL_CTX *const ssl_ctx =
			make_ssl_ctx(hardcoded_certificate_file,
				hardcoded_certificate_key_file,
				conf.openssl_security_level,
				conf.min_tls_proto_version, lerr);
		if (ssl_ctx == NULL) {
			close(fd);
			goto Error;
		}
		register_listener_cfgs_socket(fd, ssl_ctx, 1);
	}

	return 0;

Error:
	close_listener_cfgs_sockets();
	return 1;
}

/* Launched in TX thread. */
static int
load_and_handle_listen_from_lua(lua_State *L, int lua_stack_idx_table,
				const char **lerr)
{
	SSL_library_init();
	SSL_load_error_strings();

	if (load_default_listen_params(lerr) == 0)
		return 0;

	EVP_cleanup();
	ERR_free_strings();
	assert(*lerr != NULL);
	return 1;
}

/* Launched in TX thread. */
static void
reset_thread_ctx(unsigned idx)
{
	thread_ctx_t *const thread_ctx = &conf.thread_ctxs[idx];

	thread_ctx->call_from_tx_waiter = NULL;
	thread_ctx->push_from_tx_is_sleeping = false;
	thread_ctx->queue_from_tx_fd_consumed = false;
}

/* Launched in TX thread. */
static void
cleanup_h2o(thread_ctx_t *thread_ctx)
{
	h2o_evloop_destroy(thread_ctx->ctx.loop);
}

/* Launched in TX thread.
 * Returns false in case of error. */
static bool
init_worker_thread(unsigned thread_idx)
{
	thread_ctx_t *const thread_ctx = &conf.thread_ctxs[thread_idx];

	if ((thread_ctx->queue_from_tx = xtm_queue_new(QUEUE_FROM_TX_ITEMS))
	    == NULL)
		goto alloc_xtm_failed;

	if ((thread_ctx->listener_ctxs = (listener_ctx_t *)
	    malloc(conf.num_listeners * sizeof(listener_ctx_t))) == NULL)
		goto alloc_ctxs_failed;

	thread_ctx->shuttle_counter = 0;
	memset(&thread_ctx->ctx, 0, sizeof(thread_ctx->ctx));

	if (!prepare_listening_sockets(thread_ctx))
		goto prepare_listening_sockets_failed;

	return true;

prepare_listening_sockets_failed:
	close_listening_sockets(thread_ctx);
	cleanup_h2o(thread_ctx);

	free(thread_ctx->listener_ctxs);
alloc_ctxs_failed:
	my_xtm_delete_queue_from_tx(thread_ctx);
alloc_xtm_failed:
	return false;
}

/* Launched in TX thread. */
static void
deinit_worker_thread(unsigned thread_idx)
{
	thread_ctx_t *const thread_ctx = &conf.thread_ctxs[thread_idx];

	h2o_evloop_t *const loop = thread_ctx->ctx.loop;
	h2o_evloop_run(loop, 0); /* To actually free memory. */

	cleanup_h2o(thread_ctx);

	my_xtm_delete_queue_from_tx(thread_ctx);
	free(thread_ctx->listener_ctxs);
	assert(thread_ctx->shuttle_counter == 0);
}

/* Launched in TX thread. */
static int
tx_fiber_func(va_list ap)
{
	const unsigned fiber_idx = va_arg(ap, unsigned);
	/* This fiber processes requests from particular thread */
	thread_ctx_t *const thread_ctx = &conf.thread_ctxs[fiber_idx];
	struct xtm_queue *const queue_to_tx = thread_ctx->queue_to_tx;
	const int pipe_fd = xtm_queue_consumer_fd(queue_to_tx);
	while (true)
		if (coio_wait(pipe_fd, COIO_READ, DBL_MAX) & COIO_READ)
			invoke_all_in_tx(queue_to_tx);
	return 0;
}

/* Launched in TX thread. */
static void
dispose_h2o_configs(void)
{
	unsigned thr_idx;
	for (thr_idx = 0; thr_idx < MAX_threads; ++thr_idx)
		h2o_config_dispose(&conf.thread_ctxs[thr_idx].globalconf);
}

/* Launched in TX thread. */
static void
prepare_thread_ctx(unsigned thread_idx)
{
	thread_ctx_t *const thread_ctx = &conf.thread_ctxs[thread_idx];
	thread_ctx->idx = thread_idx;
	thread_ctx->listeners_created = 0;
	thread_ctx->num_connections = 0;
}

/* Launched in TX thread. */
static inline void
prepare_thread_ctxs(void)
{
	unsigned idx;
	for (idx = 0; idx < MAX_threads; ++idx)
		prepare_thread_ctx(idx);
}

/* Launched in TX thread. */
static const char *
start_worker_threads(unsigned *thr_launch_idx_ptr,
	unsigned desired_thread_count)
{
	const char *lerr;
	unsigned thr_launch_idx = *thr_launch_idx_ptr;
	for (; thr_launch_idx < desired_thread_count; ++thr_launch_idx) {
		thread_ctx_t *const thread_ctx =
			&conf.thread_ctxs[thr_launch_idx];
		if (pthread_create(&thread_ctx->tid,
		    NULL, worker_func, (void *)(uintptr_t)thr_launch_idx)) {
			lerr = "Failed to launch worker threads";
			goto Done;
		}
	}
	lerr = NULL;

Done:
	*thr_launch_idx_ptr = thr_launch_idx;
	return lerr;
}

/* Launched in TX thread. */
static void
terminate_tx_fibers(unsigned start_idx, unsigned start_idx_plus_len)
{
	/* Stopgap until next PR. */
	fprintf(stderr, "fibers termination not implemented\n");
	abort();
}

/* Launched in TX thread. */
static const char *
start_tx_fibers(unsigned *fiber_idx_ptr, unsigned start_idx_plus_len)
{
	unsigned fiber_idx = *fiber_idx_ptr;
	for (; fiber_idx < start_idx_plus_len; ++fiber_idx) {
		reset_thread_ctx(fiber_idx);

		char name[32];
		sprintf(name, "tx_h2o_fiber_%u", fiber_idx);
		struct fiber * *const fiber_ptr =
			&conf.tx_fiber_ptrs[fiber_idx];
		if ((*fiber_ptr = fiber_new(name, tx_fiber_func)) == NULL) {
			*fiber_idx_ptr = fiber_idx;
			return "Failed to create fiber";
		}
		fiber_set_joinable(*fiber_ptr, true);
		fiber_start(*fiber_ptr, fiber_idx);
	}
	*fiber_idx_ptr = fiber_idx;
	return NULL;
}

/* Launched in TX thread. */
static void
terminate_and_join_threads(unsigned start_idx, unsigned start_idx_plus_len)
{
	/* Stopgap until next PR. */
	fprintf(stderr, "http server threads termination not implemented\n");
	abort();
}

/* Launched in TX thread. */
static void
deinit_worker_threads(unsigned start_idx, unsigned start_idx_plus_len)
{
	unsigned idx;
	for (idx = start_idx; idx < start_idx_plus_len; ++idx)
		deinit_worker_thread(idx);
}

/* Launched in TX thread. */
static void
xtm_delete_queues_to_tx(unsigned start_idx, unsigned start_idx_plus_len)
{
	unsigned idx;
	for (idx = start_idx; idx < start_idx_plus_len; ++idx)
		xtm_queue_delete(conf.thread_ctxs[idx].queue_to_tx,
			XTM_QUEUE_MUST_CLOSE_PRODUCER_READFD |
			XTM_QUEUE_MUST_CLOSE_CONSUMER_READFD);
}

/* Launched in TX thread. */
static const char *
create_xtm_queues_to_tx(unsigned *xtm_to_tx_idx_ptr,
	unsigned start_idx_plus_len)
{
	const char *lerr;
	unsigned xtm_to_tx_idx = *xtm_to_tx_idx_ptr;
	for (; xtm_to_tx_idx < start_idx_plus_len; ++xtm_to_tx_idx)
		if ((conf.thread_ctxs[xtm_to_tx_idx].queue_to_tx =
		    xtm_queue_new(QUEUE_TO_TX_ITEMS)) == NULL) {
			lerr = "Failed to create xtm queue";
			goto Done;
		}
	lerr = NULL;
Done:
	*xtm_to_tx_idx_ptr = xtm_to_tx_idx;
	return lerr;
}

/* Launched in TX thread. */
static const char *
get_handler_not_reconfig(lua_State *L, int idx, unsigned *lua_site_count_ptr)
{
	lua_getfield(L, idx, "handler");
	if (lua_isnil(L, -1))
		return NULL;
	if (lua_type(L, -1) != LUA_TFUNCTION &&
	    lua_type(L, -1) != LUA_TTABLE)
		return "handler is not a function or table (router object)";

	*lua_site_count_ptr = 1;
	if (lua_type(L, -1) == LUA_TFUNCTION)
		register_lua_handler(luaL_ref(L, LUA_REGISTRYINDEX));
	else {
		return "handler is not a function";
	}
	return NULL;
}

/* Launched in TX thread. */
static inline void
apply_max_body_len(uint64_t max_body_len)
{
	unsigned thread_idx;
	for (thread_idx = 0; thread_idx < MAX_threads; ++thread_idx)
		conf.thread_ctxs[thread_idx].globalconf
			.max_request_entity_size = max_body_len;
}

/* Launched in TX thread. */
static inline void
apply_new_config(uint64_t max_body_len, uint64_t max_conn_per_thread,
	shuttle_count_t max_shuttles_per_thread
	)
{
	conf.max_conn_per_thread = max_conn_per_thread;
	conf.max_shuttles_per_thread = max_shuttles_per_thread;
	/* num_accepts is a performance optimization copy-pasted from h2o.
	 * Not yet benchmarked. */
	conf.num_accepts = max_conn_per_thread / 16;
	if (conf.num_accepts < 8)
		conf.num_accepts = 8;
	apply_max_body_len(max_body_len);
}

/* Launched in TX thread. */
static const char *
register_hosts(void)
{
	unsigned idx;
	for (idx = 0; idx < MAX_threads; ++idx)
		h2o_config_init(&conf.thread_ctxs[idx].globalconf);

	for (idx = 0; idx < MAX_threads; ++idx) {
		thread_ctx_t *const thread_ctx = &conf.thread_ctxs[idx];
		/* FIXME: Should make customizable. */
		if ((thread_ctx->hostconf =
		    h2o_config_register_host(&thread_ctx->globalconf,
			    h2o_iovec_init(H2O_STRLIT("default")),
			    H2O_DEFAULT_PORT_FOR_PROTOCOL_USED)) == NULL)
			return "libh2o host registration failed";
	}
	return NULL;
}

/* Launched in TX thread. */
static const char *
configure_handler_security_listen(lua_State *L, int idx,
	unsigned *lua_site_count_ptr
	)
{
	const char *lerr;
	unsigned lua_site_count = 0;

	assert(*lua_site_count_ptr == 0);
	if ((lerr = get_handler_not_reconfig(L, idx, &lua_site_count)) != NULL)
		return lerr;

	*lua_site_count_ptr = lua_site_count;
	if (
		lua_site_count == 0)
		return "No handlers specified";

	if (lua_site_count != 0 && conf.shuttle_size < sizeof(shuttle_t) +
	    sizeof(lua_handler_state_t))
		return "shuttle_size is too small for Lua handlers";

	if (load_and_handle_listen_from_lua(L, idx, &lerr) != 0)
		return lerr;

	return NULL;
}

/* Launched in TX thread. */
static void
unref_on_config_failure(lua_State *L,
	unsigned lua_site_count)
{
	unsigned idx;
	for (idx = 0; idx < lua_site_count; ++idx)
		luaL_unref(L, LUA_REGISTRYINDEX,
			conf.lua_handler_ref);
}

/* N. b.: This macro uses goto. */
#define PROCESS_OPTIONAL_PARAM(name) \
	lua_getfield(L, LUA_STACK_IDX_TABLE, #name); \
	uint64_t name; \
	if (is_nil_or_null(L, -1)) \
		name = DEFAULT_##name; \
	else { \
		name = my_lua_tointegerx(L, -1, &is_integer); \
		if (!is_integer) { \
			lerr = "parameter " #name " is not a number"; \
			goto error_parameter_not_a_number; \
		} \
		if (name > MAX_##name) { \
			name = MAX_##name; \
			fprintf(stderr, "Warning: parameter \"" #name "\" " \
				"adjusted to %llu (upper limit)\n", \
				(unsigned long long)name); \
		} else if (name < MIN_##name) { \
			name = MIN_##name; \
			fprintf(stderr, "Warning: parameter \"" #name "\" " \
				"adjusted to %llu (lower limit)\n", \
				(unsigned long long)name); \
		} \
	}

#define PROCESS_MAX_BODY_LEN() \
	const uint64_t max_body_len = shuttle_size - sizeof(shuttle_t);

/* N. b.: This macro uses goto. */
#define PROCESS_OPTIONAL_PARAMS() \
	int is_integer; \
	PROCESS_OPTIONAL_PARAM(threads); \
	PROCESS_OPTIONAL_PARAM(max_conn_per_thread); \
	PROCESS_OPTIONAL_PARAM(max_shuttles_per_thread); \
	PROCESS_OPTIONAL_PARAM(shuttle_size); \
	PROCESS_MAX_BODY_LEN(); \
	/* FIXME: Maybe we need to configure tfo_queues? */

/* Launched in TX thread. */
static inline void
save_params_to_conf(unsigned shuttle_size, unsigned threads)
{
	conf.num_threads = threads;
	conf.shuttle_size = shuttle_size;

	conf.max_headers_lua = (conf.shuttle_size - sizeof(shuttle_t) -
		offsetof(lua_handler_state_t, un.resp.first.headers)) /
		sizeof(http_header_entry_t);
	conf.max_path_len_lua = conf.shuttle_size - sizeof(shuttle_t) -
		offsetof(lua_handler_state_t, un.req.buffer);
}

/* Launched in TX thread. */
static int
cfg(lua_State *L)
{
	/* Lua parameters: cfg table. */
	enum {
		LUA_STACK_IDX_TABLE = 1,
		LUA_STACK_REQUIRED_PARAMS_COUNT = LUA_STACK_IDX_TABLE,
	};
	const char *lerr = NULL; /* Error message for caller. */

	if (lua_gettop(L) < LUA_STACK_REQUIRED_PARAMS_COUNT) {
		lerr = "No parameters specified";
		goto error_no_parameters;
	}

	assert(!conf.cfg_in_progress);
	conf.cfg_in_progress = true;
	if (conf.configured)
	{
		lerr = "Server is already launched";
		goto error_something;
	}

	/* N. b.: This macro uses goto. */
	PROCESS_OPTIONAL_PARAMS();

	save_params_to_conf(shuttle_size, threads);

	if ((conf.thread_ctxs = (thread_ctx_t *)malloc(MAX_threads *
	    sizeof(thread_ctx_t))) == NULL) {
		lerr = "Failed to allocate memory for thread contexts";
		goto thread_ctxs_alloc_failed;
	}

	prepare_thread_ctxs();

	if ((lerr = register_hosts()) != NULL)
		goto register_host_failed;

	unsigned lua_site_count = 0;

	if ((lerr = configure_handler_security_listen(L, LUA_STACK_IDX_TABLE,
	    &lua_site_count
	    )) != NULL)
		goto invalid_handler;

	unsigned xtm_to_tx_idx = 0;
	if ((lerr = create_xtm_queues_to_tx(&xtm_to_tx_idx, conf.num_threads))
	    != NULL)
		goto xtm_to_tx_fail;

	unsigned fiber_idx = 0;
	if ((lerr = start_tx_fibers(&fiber_idx, conf.num_threads)) != NULL)
		goto fibers_fail;
	unsigned thr_init_idx = 0;
	for (; thr_init_idx < conf.num_threads; ++thr_init_idx)
		if (!init_worker_thread(thr_init_idx)) {
			lerr = "Failed to init worker threads";
			goto threads_init_fail;
		}

	apply_new_config(max_body_len, max_conn_per_thread,
		max_shuttles_per_thread
		);

	unsigned thr_launch_idx = 0;
	if ((lerr = start_worker_threads(&thr_launch_idx, conf.num_threads)) !=
	    NULL)
		goto threads_launch_fail;

	conf.configured = true;
	conf.cfg_in_progress = false;
	return 0;

threads_launch_fail:
	terminate_and_join_threads(0, thr_launch_idx);
threads_init_fail:
	deinit_worker_threads(0, thr_init_idx);
fibers_fail:
	terminate_tx_fibers(0, fiber_idx);
xtm_to_tx_fail:
	xtm_delete_queues_to_tx(0, xtm_to_tx_idx);
	close_listener_cfgs_sockets();
	deinit_listener_cfgs();
invalid_handler:
	unref_on_config_failure(L, lua_site_count);
register_host_failed:
	/* N.b.: h2o currently can't "unregister" host(s). */
	dispose_h2o_configs();
	free(conf.thread_ctxs);
thread_ctxs_alloc_failed:
error_parameter_not_a_number:
error_something:
error_no_parameters:
	conf.cfg_in_progress = false;
	assert(lerr != NULL);
	return luaL_error(L, lerr);
}

#undef PROCESS_OPTIONAL_PARAMS
#undef PROCESS_OPTIONAL_PARAM

static const struct luaL_Reg mylib[] = {
	{"cfg", cfg},
	{NULL, NULL}
};

int
luaopen_httpng(lua_State *L)
{
	box_null_cdata_type = luaL_ctypeid(L, "void *");

	luaL_newlib(L, mylib);
	return 1;
}
