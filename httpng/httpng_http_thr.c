#include "httpng_private.h"
#include <h2o/serverutil.h>
#include "../third_party/h2o/deps/cloexec/cloexec.h"

#include <fcntl.h>
#include <poll.h>

#define container_of my_container_of

/* Launched in HTTP(S) server thread. */
static inline void
invoke_all_in_http_thr(struct xtm_queue *queue)
{
	/* FIXME: Maybe we should log consume error (should never happen?) */
	(void)xtm_queue_consume(xtm_queue_consumer_fd(queue));

	(void)xtm_queue_invoke_funs_all(queue);
	if (xtm_queue_get_reset_was_full(queue))
		while (xtm_queue_notify_producer(queue) != 0) {
			/* FIXME: Maybe we should log error
			 * (should not happen normally)? */
			usleep(1000);
		}
}

/* Launched in HTTP server thread. */
static inline thread_ctx_t *
get_curr_thread_ctx(void)
{
	return curr_thread_ctx;
}

/* Launched in HTTP(S) server thread. */
static inline struct xtm_queue *
get_queue_to_tx(void)
{
	return get_curr_thread_ctx()->queue_to_tx;
}

/* Launched in HTTP server thread. */
static inline lua_handler_state_t *
get_lua_handler_state(h2o_generator_t *generator)
{
	return container_of(generator,
		lua_handler_state_t, un.resp.any.generator);
}

/* Launched in HTTP server thread. */
static inline void
reliably_notify_xtm_consumer_from_http_thr(struct xtm_queue *queue)
{
	while (xtm_queue_notify_consumer(queue) != 0) {
		/* Actually notification should never fail, but... */
		assert(false);
		usleep(1000);
	}
}

/* Launched in HTTP(S) server thread.
 * N. b.: xtm queue is single-producer, there should be no pushes to the same
 * queue from TX or another HTTP(S) server thread until we are done. */
static void
call_from_http_thr(struct xtm_queue *queue, void *func, void *param)
{
	while (xtm_queue_push_fun(queue, (void (*)(void *))func, param,
	    XTM_QUEUE_PRODUCER_NEEDS_NOTIFICATIONS) != 0) {
		const int fd = xtm_queue_producer_fd(queue);
		struct pollfd fds[] = {
			{
				.fd = fd,
				.events = POLLIN,
				.revents = 0,
			},
		};
		(void)poll(fds, 1, -1);
		if (fds[0].revents & POLLIN)
			/* FIXME: Maybe we should log consume error
			 * (should never happen?) */
			(void)xtm_queue_consume(fd);
	}
	reliably_notify_xtm_consumer_from_http_thr(queue);
}

/* Launched in HTTP(S) server thread. */
static inline void
call_in_tx_with_shuttle(shuttle_func_t *func, shuttle_t *shuttle)
{
	assert(shuttle->thread_ctx == get_curr_thread_ctx());
	call_from_http_thr(get_queue_to_tx(), func, shuttle);
}

/* Launched in HTTP(S) server thread. */
static inline void
call_in_tx_with_lua_handler_state(lua_handler_state_func_t *func,
	lua_handler_state_t *param)
{
	assert(get_shuttle(param)->thread_ctx == get_curr_thread_ctx());
	call_from_http_thr(get_queue_to_tx(), func, param);
}

/* Launched in HTTP server thread. */
static void
free_shuttle_lua(shuttle_t *shuttle)
{
	shuttle->disposed = true;
	call_in_tx_with_shuttle(cancel_processing_lua_req_in_tx,
		shuttle);
}

/* Launched in HTTP server thread. */
static inline void
call_in_tx_continue_processing_lua_req(lua_handler_state_t *state)
{
	call_in_tx_with_lua_handler_state(continue_processing_lua_req_in_tx,
		state);
}

/* Launched in HTTP server thread when H2O has sent everything
 * and asks for more. */
static void
proceed_sending_lua(h2o_generator_t *self, h2o_req_t *req)
{
	call_in_tx_continue_processing_lua_req(get_lua_handler_state(self));
}

/* Launched in HTTP server thread. */
static inline void
send_lua(h2o_req_t *req, lua_handler_state_t *state)
{
	h2o_iovec_t buf;
	buf.base = (char *)state->un.resp.any.payload;
	buf.len = state->un.resp.any.payload_len;
	h2o_send(req, &buf, 1, state->un.resp.any.is_last_send
		? H2O_SEND_STATE_FINAL : H2O_SEND_STATE_IN_PROGRESS);
}

/* Launched in HTTP server thread. */
static inline void
fill_headers(lua_handler_state_t *state, h2o_req_t *req)
{
	const unsigned num_headers = state->un.resp.first.num_headers;
	unsigned header_idx;
	for (header_idx = 0; header_idx < num_headers; ++header_idx) {
		const http_header_entry_t *const header =
			&state->un.resp.first.headers[header_idx];
		h2o_add_header_by_str(&req->pool, &req->res.headers,
			header->name, header->name_len,

			/* FIXME: Should benchmark whether this
			 * faster than 0. */
			1,

			NULL, /* FIXME: Do we need orig_name? */
			header->value, header->value_len);
	}
}

/* Launched in HTTP server thread to postprocess first response
 * (with HTTP headers). */
void
postprocess_lua_req_first(shuttle_t *shuttle)
{
	if (shuttle->disposed)
		return;
	lua_handler_state_t *const state =
		(lua_handler_state_t *)(&shuttle->payload);
	h2o_req_t *const req = shuttle->never_access_this_req_from_tx_thread;
	req->res.status = state->un.resp.first.http_code;
	req->res.reason = "OK"; /* FIXME: Customizable? */
	fill_headers(state, req);

	state->un.resp.any.generator = (h2o_generator_t){
		proceed_sending_lua,

		/* Do not use stop_sending, we handle everything
		 * in free_shuttle_lua(). */
		NULL
	};
	req->res.content_length = state->un.resp.first.content_length;
	h2o_start_response(req, &state->un.resp.any.generator);
	send_lua(req, state);
}

/* Launched in HTTP server thread to postprocess response (w/o HTTP headers) */
void
postprocess_lua_req_others(shuttle_t *shuttle)
{
	lua_handler_state_t *const state =
		(lua_handler_state_t *)(&shuttle->payload);
	if (shuttle->disposed)
		return;
	h2o_req_t *const req = shuttle->never_access_this_req_from_tx_thread;
	send_lua(req, state);
}

/* Launched in HTTP server thread.
 * Returns normalized authority ("virtual host" name) length. */
static inline unsigned
prepare_authority(h2o_req_t *req)
{
	assert(req->authority.len >= 0);
	assert(req->authority.len <= INT_MAX);
	unsigned authority_len = req->authority.len;
	/* Formats: "foo.tarantool.io", "foo.tarantool.io:8443",
	 * cut port number if present. */
	int pos = authority_len - 1;
	while (pos >= 0) {
		const char c = req->authority.base[pos];
		if (c == ':') {
			authority_len = pos;
			break;
		}
		if (c == '.')
			/* Just optimization. */
			break;
		--pos;
	}
	return authority_len;
}

/* Launched in HTTP server thread. */
void
lua_req_handler_ex(h2o_req_t *req,
	shuttle_t *shuttle, unsigned router_data_len)
{
	lua_handler_state_t *const state =
		(lua_handler_state_t *)&shuttle->payload;
	if ((state->un.req.method_len = req->method.len) >
	    sizeof(state->un.req.method)) {
		/* Error. */
		free_shuttle_with_anchor(shuttle);
		req->res.status = 500;
		req->res.reason = "Method name is too long";
		h2o_send_inline(req, H2O_STRLIT("Method name is too long\n"));
		return;
	}
	unsigned current_offset = router_data_len +
		(state->un.req.path_len = req->path.len);
	state->un.req.authority_len = prepare_authority(req);
	if (current_offset + state->un.req.authority_len >
	    conf.max_path_len_lua) {
		/* Error. */
		free_shuttle_with_anchor(shuttle);
		req->res.status = 500;
		req->res.reason = "Request is too long";
		h2o_send_inline(req, H2O_STRLIT("Request is too long\n"));
		return;
	}

	state->un.req.router_data_len = router_data_len;

	memcpy(state->un.req.method, req->method.base,
		state->un.req.method_len);
	memcpy(&state->un.req.buffer[router_data_len], req->path.base,
		state->un.req.path_len);
	memcpy(&state->un.req.buffer[current_offset], req->authority.base,
		state->un.req.authority_len);
	current_offset += state->un.req.authority_len;

	STATIC_ASSERT(LUA_QUERY_NONE <
		(1ULL << (8 * sizeof(state->un.req.query_at))),
		".query_at field is not large enough to store LUA_QUERY_NONE");
	state->un.req.query_at = (req->query_at == SIZE_MAX)
		? LUA_QUERY_NONE : req->query_at;
	state->un.req.version_major = req->version >> 8;
	state->un.req.version_minor = req->version & 0xFF;

	const h2o_header_t *const headers = req->headers.entries;
	const size_t num_headers = req->headers.size;
	/* state->un.req.buffer[] format:
	 *
	 * char router_data[router_data_len]
	 * char path[state->un.req.path.len]
	 * char authority[state->un.req.authority_len]
	 * FIXME: Alignment to at least to header_offset_t should be here
	 *   (compatibility/performance).
	 * received_http_header_handle_t handles[num_headers]
	 * {repeat num_headers times} char name[handles[i].name_size], '\0',
	 *   char value[handles[i].value_size]
	 * char body[]
	 *
	 * '\0' is for lua_setfield().
	 * */
	const unsigned max_offset = conf.max_path_len_lua;
	const size_t headers_size = num_headers *
		sizeof(received_http_header_handle_t);
	const unsigned headers_payload_offset = current_offset + headers_size;
	if (headers_payload_offset > max_offset) {
	TooLargeHeaders:
		/* Error. */
		free_shuttle_with_anchor(shuttle);
		req->res.status = 431;
		req->res.reason = "Request Header Fields Too Large";
		h2o_send_inline(req,
			H2O_STRLIT("Request Header Fields Too Large\n"));
		return;
	}
	received_http_header_handle_t *const handles =
		(received_http_header_handle_t *)&state->un.req.buffer[
			current_offset];
	current_offset += num_headers * sizeof(received_http_header_handle_t);
	unsigned header_idx;
	for (header_idx = 0; header_idx < num_headers; ++header_idx) {
		const h2o_header_t *const header = &headers[header_idx];
		if (current_offset + header->name->len + 1 +
		    header->value.len > max_offset)
			goto TooLargeHeaders;
		received_http_header_handle_t *const handle =
			&handles[header_idx];
		handle->name_size = header->name->len;
		handle->value_size = header->value.len;
		memcpy(&state->un.req.buffer[current_offset],
			header->name->base, handle->name_size);
		current_offset += handle->name_size;
		state->un.req.buffer[current_offset] = 0;
		++current_offset;
		memcpy(&state->un.req.buffer[current_offset],
			header->value.base, handle->value_size);
		current_offset += handle->value_size;
	}

	unsigned body_bytes_to_copy;
	if (current_offset + req->entity.len > max_offset) {
			/* Error. */
			free_shuttle_with_anchor(shuttle);
			req->res.status = 413;
			req->res.reason = "Payload Too Large";
			h2o_send_inline(req,
				H2O_STRLIT("Payload Too Large\n"));
			return;
	} else
		body_bytes_to_copy = req->entity.len;

	state->un.req.num_headers = num_headers;
	state->un.req.body_len = body_bytes_to_copy;
	memcpy(&state->un.req.buffer[current_offset],
		req->entity.base, body_bytes_to_copy);

	state->sent_something = false;
	state->cancelled = false;
	state->waiter = NULL;

	struct xtm_queue *const queue = get_queue_to_tx();
	if (xtm_queue_push_fun(queue,
	    (void(*)(void *))&process_lua_req_in_tx, shuttle, 0) != 0) {
		/* Error */
		free_shuttle_with_anchor(shuttle);
		req->res.status = 500;
		req->res.reason = "Queue overflow";
		h2o_send_inline(req, H2O_STRLIT("Queue overflow\n"));
		return;
	}
	reliably_notify_xtm_consumer_from_http_thr(queue);
	shuttle->anchor->user_free_shuttle = &free_shuttle_lua;
}

/* Launched in HTTP server thread. */
int
lua_req_handler(lua_h2o_handler_t *self, h2o_req_t *req)
{
	shuttle_t *const shuttle = prepare_shuttle(req);
	if (shuttle != NULL)
		lua_req_handler_ex(req, shuttle, 0);
	return 0;
}

/* Launched in HTTP server thread. */
static inline shuttle_t *
alloc_shuttle(thread_ctx_t *thread_ctx)
{
	STATIC_ASSERT(sizeof(shuttle_count_t) >= sizeof(unsigned),
		"MAX_max_shuttles_per_thread may be too large");
	if (++thread_ctx->shuttle_counter > conf.max_shuttles_per_thread) {
		--thread_ctx->shuttle_counter;
		return NULL;
	}
	/* FIXME: Use per-thread pools */
	shuttle_t *const shuttle = (shuttle_t *)malloc(conf.shuttle_size);
	return shuttle;
}

/* Launched in HTTP server thread.
 * Should only be called if disposed==false.
 * Expected usage: when req handler can't or wouldn't queue request
 * to TX thread. */
void
free_shuttle_with_anchor(shuttle_t *shuttle)
{
	assert(!shuttle->disposed);
	shuttle->anchor->shuttle = NULL;
	free_shuttle(shuttle);
}

/* Launched in HTTP server thread. */
static inline void
anchor_dispose(void *param)
{
	anchor_t *const anchor = (anchor_t*)param;
	shuttle_t *const shuttle = anchor->shuttle;
	if (shuttle != NULL) {
		if (anchor->user_free_shuttle != NULL)
			anchor->user_free_shuttle(shuttle);
		else
			shuttle->disposed = true;
	}
}

/* Launched in HTTP server thread. */
shuttle_t *
prepare_shuttle(h2o_req_t *req)
{
	thread_ctx_t *const thread_ctx = get_curr_thread_ctx();
	shuttle_t *const shuttle = alloc_shuttle(thread_ctx);
	if (shuttle == NULL) {
		req->res.status = 500;
		req->res.reason = "No memory";
		h2o_send_inline(req, H2O_STRLIT("No memory\n"));
		return NULL;
	}
	anchor_t *const anchor = (anchor_t *)h2o_mem_alloc_shared(&req->pool,
		sizeof(anchor_t), &anchor_dispose);
	anchor->user_free_shuttle = NULL;
	shuttle->anchor = anchor;
	anchor->shuttle = shuttle;
	shuttle->never_access_this_req_from_tx_thread = req;
	shuttle->thread_ctx = thread_ctx;
	shuttle->disposed = false;
	return shuttle;
}

/* Launched in HTTP server thread. */
static void
on_underlying_socket_free(void *data)
{
	thread_ctx_t *const thread_ctx = get_curr_thread_ctx();
	--thread_ctx->num_connections;
}

/* Launched in HTTP server thread. */
static void
on_call_from_tx(h2o_socket_t *listener, const char *err)
{
	if (err != NULL)
		return;

	invoke_all_in_http_thr(get_curr_thread_ctx()->queue_from_tx);
}

/* Launched in HTTP server thread. */
static void
on_accept(h2o_socket_t *listener, const char *err)
{
	if (err != NULL)
		return;

	listener_ctx_t *const listener_ctx = (listener_ctx_t *)listener->data;
	thread_ctx_t *const thread_ctx = get_curr_thread_ctx();
	unsigned remain = conf.num_accepts;

	do {
		if (thread_ctx->num_connections >= conf.max_conn_per_thread)
			break;
		h2o_socket_t *const sock = h2o_evloop_socket_accept(listener);
		if (sock == NULL)
			return;

		++thread_ctx->num_connections;

		h2o_socket_t *const h2o_sock = sock;
		h2o_sock->on_close.cb = on_underlying_socket_free;
		h2o_sock->on_close.data = sock;

		h2o_accept(&listener_ctx->accept_ctx, h2o_sock);
	} while (--remain);
}

/* Launched in HTTP server thread. */
static inline void
listening_sockets_start_read(thread_ctx_t *thread_ctx)
{
	unsigned listener_idx;
	for (listener_idx = 0; listener_idx < thread_ctx->listeners_created;
	    ++listener_idx) {
		listener_ctx_t *const listener_ctx =
		   &thread_ctx->listener_ctxs[listener_idx];
		listener_ctx->sock =
			h2o_evloop_socket_create(thread_ctx->ctx.loop,
			listener_ctx->fd, H2O_SOCKET_FLAG_DONT_READ);
		listener_ctx->sock->data = listener_ctx;
		h2o_socket_read_start(listener_ctx->sock, on_accept);
	}
}

/* This is HTTP server thread main function. */
void *
worker_func(void *param)
{
	const unsigned thread_idx = (unsigned)(uintptr_t)param;
	thread_ctx_t *const thread_ctx = &conf.thread_ctxs[thread_idx];
	curr_thread_ctx = thread_ctx;
	h2o_context_init(&thread_ctx->ctx, h2o_evloop_create(),
		&thread_ctx->globalconf);

	thread_ctx->sock_from_tx =
		h2o_evloop_socket_create(thread_ctx->ctx.loop,
			xtm_queue_consumer_fd(thread_ctx->queue_from_tx),
			H2O_SOCKET_FLAG_DONT_READ);

	h2o_socket_read_start(thread_ctx->sock_from_tx, on_call_from_tx);
	thread_ctx->queue_from_tx_fd_consumed = true;
	listening_sockets_start_read(thread_ctx);
	h2o_evloop_t *loop = thread_ctx->ctx.loop;
	while (true)
		h2o_evloop_run(loop, INT32_MAX);

	return NULL;
}
