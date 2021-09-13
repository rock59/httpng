#include "httpng_private.h"
#include <h2o/serverutil.h>
#ifdef SUPPORT_CONN_LIST
#ifndef USE_LIBUV
#include <h2o/evloop_socket.h>
#endif /* USE_LIBUV */
#endif /* SUPPORT_CONN_LIST */
#ifdef SUPPORT_WEBSOCKETS
#include <h2o/websocket.h>
#endif /* SUPPORT_WEBSOCKETS */
#include "../third_party/h2o/deps/cloexec/cloexec.h"

#include <fcntl.h>
#include <poll.h>

#define container_of my_container_of

#ifdef SUPPORT_CONN_LIST
typedef struct {
#ifdef USE_LIBUV
	uv_tcp_t
#else /* USE_LIBUV */
	struct st_h2o_evloop_socket_t
#endif /* USE_LIBUV */
		super;
	h2o_linklist_t accepted_list;
} our_sock_t;
#endif /* SUPPORT_CONN_LIST */

#ifdef SUPPORT_LISTEN
#ifndef NDEBUG
static const char msg_cant_switch_ssl_ctx[] =
	"Error while switching SSL context after scanning TLS SNI";
#endif /* NDEBUG */
#endif /* SUPPORT_LISTEN */

#ifdef SUPPORT_CONN_LIST
/* Launched in HTTP server thread. */
static inline void
h2o_linklist_insert_fast(h2o_linklist_t *pos, h2o_linklist_t *node)
{
    node->prev = pos->prev;
    node->next = pos;
    node->prev->next = node;
    node->next->prev = node;
}

/* Launched in HTTP server thread. */
static inline void
h2o_linklist_unlink_fast(h2o_linklist_t *node)
{
    node->next->prev = node->prev;
    node->prev->next = node->next;
}
#endif /* SUPPORT_CONN_LIST */

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

#ifdef SUPPORT_WEBSOCKETS
/* Launched in HTTP(S) server thread. */
static inline void
call_in_tx_with_recv_data(recv_data_func_t *func, recv_data_t *recv_data)
{
	assert(recv_data->parent_shuttle->thread_ctx == get_curr_thread_ctx());
	call_from_http_thr(get_queue_to_tx(), func, recv_data);
}
#endif /* SUPPORT_WEBSOCKETS */

/* Launched in HTTP server thread. */
static inline void
call_in_tx_with_thread_ctx(thread_ctx_func_t *func, thread_ctx_t *thread_ctx)
{
	assert(thread_ctx == get_curr_thread_ctx());
	call_from_http_thr(thread_ctx->queue_to_tx, func, thread_ctx);
}

#ifdef SUPPORT_WEBSOCKETS
/* Launched in HTTP server thread. */
static inline recv_data_t *
alloc_recv_data(void)
{
	/* FIXME: Use per-thread pools? */
	recv_data_t *const recv_data = (recv_data_t *)
		malloc(conf.recv_data_size);
	return recv_data;
}
#endif /* SUPPORT_WEBSOCKETS */

#ifdef SUPPORT_WEBSOCKETS
/* Launched in HTTP server thread. */
static inline recv_data_t *
prepare_websocket_recv_data(shuttle_t *parent, unsigned payload_bytes)
{
	recv_data_t *const recv_data = alloc_recv_data();
	if (recv_data == NULL)
		return NULL;
	recv_data->parent_shuttle = parent;
	recv_data->payload_bytes = payload_bytes;
	return recv_data;
}
#endif /* SUPPORT_WEBSOCKETS */

/* Launched in HTTP server thread. */
static void
free_shuttle_lua(shuttle_t *shuttle)
{
#ifdef SUPPORT_WEBSOCKETS
	lua_handler_state_t *const state =
		(lua_handler_state_t *)(&shuttle->payload);
	if (!state->upgraded_to_websocket)
#endif /* SUPPORT_WEBSOCKETS */
	{
		shuttle->disposed = true;
		call_in_tx_with_shuttle(cancel_processing_lua_req_in_tx,
			shuttle);
	}
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

#ifdef SUPPORT_WEBSOCKETS
/* Launched in HTTP server thread. */
static void
websocket_msg_callback(h2o_websocket_conn_t *conn,
	const struct wslay_event_on_msg_recv_arg *arg)
{
	shuttle_t *const shuttle = (shuttle_t*)conn->data;
	if (arg == NULL) {
	do_close:
		;
		lua_handler_state_t *const state =
			(lua_handler_state_t *)&shuttle->payload;
		assert(conn == state->ws_conn);
		h2o_websocket_close(conn);
		state->ws_conn = NULL;
		call_in_tx_with_lua_handler_state(
			cancel_processing_lua_websocket_in_tx, state);
		return;
	}

	if (wslay_is_ctrl_frame(arg->opcode))
		return;

	lua_handler_state_t *const state =
		(lua_handler_state_t *)&shuttle->payload;
	size_t bytes_remain = arg->msg_length;
	const unsigned char *pos = arg->msg;
	while (1) {
		/* FIXME: Need flag about splitting to parts.
		 * Probably should have upper limit on a number of active
		 * recv_data - we can eat A LOT of memory. */
		const unsigned bytes_to_send = bytes_remain >
			conf.max_recv_bytes_lua_websocket
			? conf.max_recv_bytes_lua_websocket : bytes_remain;
		recv_data_t *const recv_data =
			prepare_websocket_recv_data(shuttle, bytes_to_send);
		if (recv_data == NULL)
			goto do_close;
		memcpy(get_websocket_recv_location(recv_data), pos,
			bytes_to_send);
		call_in_tx_with_recv_data(
			process_lua_websocket_received_data_in_tx, recv_data);
		if (state->ws_conn == NULL)
			/* Handler has closed connection already. */
			break;
		if ((bytes_remain -= bytes_to_send) == 0)
			break;
		pos += bytes_to_send;
	}
}
#endif /* SUPPORT_WEBSOCKETS */

#ifdef SUPPORT_WEBSOCKETS
/* Launched in HTTP server thread to postprocess upgrade to WebSocket. */
void
postprocess_lua_req_upgrade_to_websocket(shuttle_t *shuttle)
{
	lua_handler_state_t *const state =
		(lua_handler_state_t *)(&shuttle->payload);
	if (shuttle->disposed)
		return;
	h2o_req_t *const req = shuttle->never_access_this_req_from_tx_thread;

	fill_headers(state, req);
	state->upgraded_to_websocket = true;
	state->ws_conn = h2o_upgrade_to_websocket(req,
		state->ws_client_key, shuttle, websocket_msg_callback);
	/* anchor_dispose()/free_shuttle_lua() will be called by h2o. */
	call_in_tx_continue_processing_lua_req(state);
}
#endif /* SUPPORT_WEBSOCKETS */

#ifdef SUPPORT_WEBSOCKETS
/* Launched in HTTP server thread. */
void
postprocess_lua_req_websocket_send_text(lua_handler_state_t *state)
{
	/* Do not check shuttle->disposed, this is a WebSocket now. */

	struct wslay_event_msg msgarg = {
		.opcode = WSLAY_TEXT_FRAME,
		.msg = (unsigned char *)state->un.resp.any.payload,
		.msg_length = state->un.resp.any.payload_len,
	};
	if (wslay_event_queue_msg(state->ws_conn->ws_ctx, &msgarg) ||
	    wslay_event_send(state->ws_conn->ws_ctx))
		state->ws_send_failed = true;
	call_in_tx_continue_processing_lua_req(state);
}
#endif /* SUPPORT_WEBSOCKETS */

#ifdef SUPPORT_WEBSOCKETS
/* Launched in HTTP server thread. */
void
close_websocket(lua_handler_state_t *const state)
{
	if (state->ws_conn != NULL) {
		h2o_websocket_close(state->ws_conn);
		state->ws_conn = NULL;
	}
	call_in_tx_continue_processing_lua_req(state);
}
#endif /* SUPPORT_WEBSOCKETS */

#ifdef SUPPORT_SPLITTING_LARGE_BODY
/* Launched in HTTP server thread. */
void
retrieve_more_body(shuttle_t *const shuttle)
{
	if (shuttle->disposed)
		return;
	lua_handler_state_t *const state =
		(lua_handler_state_t *)&shuttle->payload;
	assert(state->un.req.is_body_incomplete);
	const h2o_req_t *const req =
		shuttle->never_access_this_req_from_tx_thread;
	assert(state->un.req.offset_within_body < req->entity.len);
	const size_t bytes_still_in_req =
		req->entity.len - state->un.req.offset_within_body;
	unsigned bytes_to_copy;
	const unsigned offset = state->un.req.offset_within_body;
	if (bytes_still_in_req > conf.max_path_len_lua) {
		bytes_to_copy = conf.max_path_len_lua;
		state->un.req.offset_within_body += bytes_to_copy;
	} else {
		bytes_to_copy = bytes_still_in_req;
		state->un.req.is_body_incomplete = false;
	}
	state->un.req.body_len = bytes_to_copy;
	memcpy(&state->un.req.buffer, &req->entity.base[offset],
		bytes_to_copy);

	call_in_tx_continue_processing_lua_req(state);
}
#endif /* SUPPORT_SPLITTING_LARGE_BODY */

#ifdef SUPPORT_THR_TERMINATION
/* Launched in HTTP server thread. */
void
tx_done(thread_ctx_t *thread_ctx)
{
#ifdef USE_LIBUV
	uv_stop(&thread_ctx->loop);
#endif /* USE_LIBUV */
	thread_ctx->tx_done_notification_received = true;
#ifdef SUPPORT_GRACEFUL_THR_TERMINATION
	if (thread_ctx->do_not_exit_tx_fiber)
		return;
#endif /* SUPPORT_GRACEFUL_THR_TERMINATION */
	call_in_tx_with_thread_ctx(tell_tx_fiber_to_exit, thread_ctx);
}
#endif /* SUPPORT_THR_TERMINATION */

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
#ifdef SUPPORT_WEBSOCKETS
	const char *ws_client_key;
	(void)h2o_is_websocket_handshake(req, &ws_client_key);
	if (ws_client_key == NULL)
		state->un.req.ws_client_key_len = 0;
	else {
		state->un.req.ws_client_key_len = WS_CLIENT_KEY_LEN;
		memcpy(state->ws_client_key, ws_client_key,
			state->un.req.ws_client_key_len);
	}
#endif /* SUPPORT_WEBSOCKETS */

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
#ifdef SUPPORT_SPLITTING_LARGE_BODY
		if (conf.use_body_split) {
			state->un.req.is_body_incomplete = true;
			body_bytes_to_copy = max_offset - current_offset;
			state->un.req.offset_within_body = body_bytes_to_copy;
		} else
#endif /* SUPPORT_SPLITTING_LARGE_BODY */
		{
			/* Error. */
			free_shuttle_with_anchor(shuttle);
			req->res.status = 413;
			req->res.reason = "Payload Too Large";
			h2o_send_inline(req,
				H2O_STRLIT("Payload Too Large\n"));
			return;
		}
	} else {
#ifdef SUPPORT_SPLITTING_LARGE_BODY
		state->un.req.is_body_incomplete = false;
#endif /* SUPPORT_SPLITTING_LARGE_BODY */
		body_bytes_to_copy = req->entity.len;
	}

	state->un.req.num_headers = num_headers;
	state->un.req.body_len = body_bytes_to_copy;
	memcpy(&state->un.req.buffer[current_offset],
		req->entity.base, body_bytes_to_copy);

	state->sent_something = false;
	state->cancelled = false;
#ifdef SUPPORT_WEBSOCKETS
	state->upgraded_to_websocket = false;
#endif /* SUPPORT_WEBSOCKETS */
	state->waiter = NULL;

#ifdef SUPPORT_REQ_INFO
	socklen_t socklen = req->conn->callbacks->get_peername(req->conn,
		(struct sockaddr *)&state->peer);
	(void)socklen;
	assert(socklen <= sizeof(state->peer));
	socklen = req->conn->callbacks->get_sockname(req->conn,
		(struct sockaddr *)&state->ouraddr);
	assert(socklen <= sizeof(state->ouraddr));
	state->un.req.is_encrypted = h2o_is_req_transport_encrypted(req);
#endif /* SUPPORT_REQ_INFO */

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
	shuttle_t *const shuttle = prepare_shuttle2(req);
	if (shuttle != NULL)
		lua_req_handler_ex(req, shuttle, 0);
	return 0;
}

#ifdef SUPPORT_ROUTER
/* Launched in HTTP server thread. */
int
router_wrapper(lua_h2o_handler_t *self, h2o_req_t *req)
{
	shuttle_t *const shuttle = prepare_shuttle2(req);
	if (shuttle != NULL)
		lua_req_handler_ex(req, shuttle, 0);
	return 0;
}
#endif /* SUPPORT_ROUTER */

/* Launched in HTTP server thread. */
static inline shuttle_t *
alloc_shuttle(thread_ctx_t *thread_ctx)
{
	STATIC_ASSERT(sizeof(shuttle_count_t) >= sizeof(unsigned),
		"MAX_max_shuttles_per_thread may be too large");
#ifdef USE_SHUTTLES_MUTEX
	pthread_mutex_lock(&thread_ctx->shuttles_mutex);
#endif /* USE_SHUTTLES_MUTEX */
	if (++thread_ctx->shuttle_counter > conf.max_shuttles_per_thread) {
		--thread_ctx->shuttle_counter;
#ifdef USE_SHUTTLES_MUTEX
		pthread_mutex_unlock(&thread_ctx->shuttles_mutex);
#endif /* USE_SHUTTLES_MUTEX */
		return NULL;
	}
#ifdef USE_SHUTTLES_MUTEX
	pthread_mutex_unlock(&thread_ctx->shuttles_mutex);
#endif /* USE_SHUTTLES_MUTEX */
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

	/* Probably should implemented support for "stubborn" anchors - 
	 * optionally wait for TX processing to finish so TX thread can
	 * access h2o_req_t directly thus avoiding copying LARGE buffers,
	 * it only makes sense in very specific cases because it stalls
	 * the whole thread if such request is gone. */
}

/* Launched in HTTP server thread. */
shuttle_t *
prepare_shuttle2(h2o_req_t *req)
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
#if 0 /* Should remove after correcting sample C handlers code. */
	shuttle->stopped = false;
#endif /* 0 */
	return shuttle;
}

/* Launched in HTTP server thread. */
static void
on_underlying_socket_free(void *data)
{
#ifdef SUPPORT_CONN_LIST
	h2o_linklist_unlink_fast(&my_container_of(data,
		our_sock_t, super)->accepted_list);
#endif /* SUPPORT_CONN_LIST */
	thread_ctx_t *const thread_ctx = get_curr_thread_ctx();
	--thread_ctx->num_connections;
#ifdef USE_LIBUV
	free(data);
#endif /* USE_LIBUV */
}

#ifdef USE_LIBUV

/* Launched in HTTP server thread. */
static void
on_call_from_tx(uv_poll_t *handle, int status, int events)
{
	(void)handle;
	(void)events;
	if (status != 0)
		return;
	invoke_all_in_http_thr(get_curr_thread_ctx()->queue_from_tx);
}

#else /* USE_LIBUV */

/* Launched in HTTP server thread. */
static void
on_call_from_tx(h2o_socket_t *listener, const char *err)
{
	if (err != NULL)
		return;

	invoke_all_in_http_thr(get_curr_thread_ctx()->queue_from_tx);
}

#endif /* USE_LIBUV */

#ifdef USE_LIBUV

/* Launched in HTTP server thread. */
static void
on_accept(uv_stream_t *uv_listener, int status)
{
	if (status != 0)
		return;

	thread_ctx_t *const thread_ctx = get_curr_thread_ctx();
	if (thread_ctx->num_connections >= conf.max_conn_per_thread)
		return;

	/* FIXME: Pools instead of malloc? */
	our_sock_t *const conn = malloc(sizeof(*conn));
	if (conn == NULL)
		return;
	if (uv_tcp_init(uv_listener->loop, &conn->super)) {
		free(conn);
		return;
	}

	if (uv_accept(uv_listener, (uv_stream_t *)&conn->super)) {
		uv_close((uv_handle_t *)conn, (uv_close_cb)free);
		return;
	}

	h2o_linklist_insert_fast(&thread_ctx->accepted_sockets,
		&conn->accepted_list);
	++thread_ctx->num_connections;

	listener_ctx_t *const listener_ctx =
		(listener_ctx_t *)uv_listener->data;
	h2o_accept(&listener_ctx->accept_ctx,
		h2o_uv_socket_create((uv_stream_t *)&conn->super,
			(uv_close_cb)on_underlying_socket_free));
}

#else /* USE_LIBUV */

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
#ifdef SUPPORT_CONN_LIST
		struct st_h2o_evloop_socket_t *const sock =
			h2o_evloop_socket_accept_ex(listener,
				sizeof(our_sock_t));
#else /* SUPPORT_CONN_LIST */
		h2o_socket_t *const sock = h2o_evloop_socket_accept(listener);
#endif /* SUPPORT_CONN_LIST */
		if (sock == NULL)
			return;

#ifdef SUPPORT_CONN_LIST
		our_sock_t *const item =
			container_of(sock, our_sock_t, super);
		h2o_linklist_insert_fast(&thread_ctx->accepted_sockets,
			&item->accepted_list);
#endif /* SUPPORT_CONN_LIST */

		++thread_ctx->num_connections;

		h2o_socket_t *const h2o_sock =
#ifdef SUPPORT_CONN_LIST
			&sock->super;
#else /* SUPPORT_CONN_LIST */
			sock;
#endif /* SUPPORT_CONN_LIST */
		h2o_sock->on_close.cb = on_underlying_socket_free;
		h2o_sock->on_close.data = sock;

		h2o_accept(&listener_ctx->accept_ctx, h2o_sock);
	} while (--remain);
}

#endif /* USE_LIBUV */

#ifdef SUPPORT_THR_TERMINATION
/* Launched in HTTP server thread. */
static inline void
listening_sockets_stop_read(thread_ctx_t *thread_ctx)
{
#ifdef USE_LIBUV
#error "listening_sockets_stop_read() not implemented for libuv yet"
#else /* USE_LIBUV */
	unsigned listener_idx;
	for (listener_idx = 0; listener_idx < thread_ctx->listeners_created;
	    ++listener_idx) {
		listener_ctx_t *const listener_ctx =
		    &thread_ctx->listener_ctxs[listener_idx];
		h2o_socket_read_stop(listener_ctx->sock);
		h2o_socket_close(listener_ctx->sock);
		listener_ctx->sock = NULL;
	}
	thread_ctx->listeners_created = 0;
#endif /* USE_LIBUV */
}
#endif /* SUPPORT_THR_TERMINATION */

/* Launched in HTTP server thread. */
static inline void
listening_sockets_start_read(thread_ctx_t *thread_ctx)
{
#ifdef USE_LIBUV
#error "listening_sockets_start_read() not implemented for libuv yet"
#else /* USE_LIBUV */
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
#endif /* USE_LIBUV */
}

#ifdef SUPPORT_LISTEN
/* Launched in HTTP thread. */
int
servername_callback(SSL *s, int *al, void *arg)
{
	assert(arg != NULL);
	sni_map_t *sni_map = (servername_callback_arg_t *)arg;

	const char *servername = SSL_get_servername(s,
		TLSEXT_NAMETYPE_host_name);
	if (servername == NULL) {
#ifndef NDEBUG
		fprintf(stderr, "Server name is not received:%s\n",
			servername);
#endif /* NDEBUG */
		/* FIXME: think maybe return SSL_TLSEXT_ERR_NOACK. */
		goto get_servername_fail;
	}
	size_t servername_len = strlen(servername);

	/* FIXME: make hash table for sni_fields, not an array. */
	size_t i;
	for (i = 0; i < sni_map->sni_fields_size; ++i) {
		if (servername_len == sni_map->sni_fields[i].hostname.len &&
		    memcmp(servername, sni_map->sni_fields[i].hostname.base,
		    servername_len) == 0) {
			SSL_CTX *ssl_ctx = sni_map->sni_fields[i].ssl_ctx;
			if (SSL_set_SSL_CTX(s, ssl_ctx) == NULL) {
#ifndef NDEBUG
				fprintf(stderr, "%s\n",
					msg_cant_switch_ssl_ctx);
#endif /* NDEBUG */
				goto set_ssl_ctx_fail;
			}
		}
	}
	return SSL_TLSEXT_ERR_OK;

set_ssl_ctx_fail:
get_servername_fail:
	return SSL_TLSEXT_ERR_ALERT_FATAL;
}
#endif /* SUPPORT_LISTEN */

#ifdef SUPPORT_THR_TERMINATION
/* Launched in HTTP(S) server thread. */
static inline void
call_in_tx_finish_processing_lua_reqs(thread_ctx_t *thread_ctx)
{
	call_in_tx_with_thread_ctx(finish_processing_lua_reqs_in_tx,
		thread_ctx);
}
#endif /* SUPPORT_THR_TERMINATION */

#ifdef SUPPORT_THR_TERMINATION
/* Launched in HTTP server thread. */
static inline void
tell_close_connection(our_sock_t *item)
{
#ifdef USE_LIBUV
	struct st_h2o_uv_socket_t *const uv_sock = item->super.data;

	/* This is not really safe (st_h2o_uv_socket_t can be changed
	 * so h2o_socket_t is no longer first member - unlikely but possible)
	 * but the alternative is to include A LOT of h2o internal headers. */
	h2o_socket_t *const sock = (h2o_socket_t *)uv_sock;
#else /* USE_LIBUV */
	h2o_socket_t *const sock = &item->super.super;
#endif /* USE_LIBUV */
	h2o_close_working_socket(sock);
}
#endif /* SUPPORT_THR_TERMINATION */

#ifdef SUPPORT_THR_TERMINATION
/* Launched in HTTP server thread. */
static inline void
close_existing_connections(thread_ctx_t *thread_ctx)
{
	our_sock_t *item =
		container_of(thread_ctx->accepted_sockets.next,
			our_sock_t, accepted_list);
	while (&item->accepted_list != &thread_ctx->accepted_sockets) {
		our_sock_t *const next = container_of(item->accepted_list.next,
			our_sock_t, accepted_list);
		tell_close_connection(item);
		item = next;
	}
}
#endif /* SUPPORT_THR_TERMINATION */

#ifdef SUPPORT_THR_TERMINATION
/* Launched in HTTP server thread. */
static inline void
prepare_worker_for_shutdown(thread_ctx_t *thread_ctx
#ifdef SUPPORT_GRACEFUL_THR_TERMINATION
	, bool use_graceful_shutdown
#endif /*  SUPPORT_GRACEFUL_THR_TERMINATION */
	)
{
#ifdef SUPPORT_GRACEFUL_THR_TERMINATION
	if (use_graceful_shutdown)
		h2o_context_request_shutdown(&thread_ctx->ctx);
	else
#endif /* SUPPORT_GRACEFUL_THR_TERMINATION */
		close_existing_connections(thread_ctx);
#ifdef SUPPORT_GRACEFUL_THR_TERMINATION
	thread_ctx->do_not_exit_tx_fiber = use_graceful_shutdown;
#endif /* SUPPORT_GRACEFUL_THR_TERMINATION */

	fprintf(stderr, "Thread #%u: shutdown request received, "
		"waiting for TX processing to complete...\n",
		thread_ctx->idx);
	call_in_tx_finish_processing_lua_reqs(thread_ctx);
}
#endif /* SUPPORT_THR_TERMINATION */

#ifdef SUPPORT_THR_TERMINATION
/* Launched in HTTP server thread. */
static inline void
handle_worker_shutdown(thread_ctx_t *thread_ctx
#ifdef SUPPORT_GRACEFUL_THR_TERMINATION
	, bool use_graceful_shutdown
#endif /* SUPPORT_GRACEFUL_THR_TERMINATION */
	)
{
#ifdef SUPPORT_GRACEFUL_THR_TERMINATION
	if (!use_graceful_shutdown)
#endif /* SUPPORT_GRACEFUL_THR_TERMINATION */
		goto done;

#ifdef SUPPORT_GRACEFUL_THR_TERMINATION
	close_existing_connections(thread_ctx);

	/* There can still be requests in flight. */
	assert(thread_ctx->do_not_exit_tx_fiber);
	assert(thread_ctx->tx_done_notification_received);
	thread_ctx->tx_done_notification_received = false;
	thread_ctx->do_not_exit_tx_fiber = false;
	call_in_tx_finish_processing_lua_reqs(thread_ctx);
#ifdef USE_LIBUV
	uv_run(&thread_ctx->loop, UV_RUN_DEFAULT);
#else /* USE_LIBUV */
	h2o_evloop_t *const loop = thread_ctx->ctx.loop;
	while (!thread_ctx->tx_done_notification_received)
		h2o_evloop_run(loop, 1);
#endif /* USE_LIBUV */
#endif /* SUPPORT_GRACEFUL_THR_TERMINATION */
done:
	h2o_make_shutdown_ungraceful(&thread_ctx->ctx);
}
#endif /* SUPPORT_THR_TERMINATION */

#ifdef SUPPORT_THR_TERMINATION
/* Launched in HTTP server thread. */
static inline void
on_termination_notification(void *param)
{
	thread_ctx_t *const thread_ctx =
		my_container_of(param, thread_ctx_t, terminate_notifier);
	thread_ctx->shutdown_requested = true;
#ifdef USE_LIBUV
	uv_stop(&thread_ctx->loop);
#endif /* USE_LIBUV */
}
#endif /* SUPPORT_THR_TERMINATION */

#ifndef USE_LIBUV
#ifdef SUPPORT_THR_TERMINATION
/* Launched in HTTP server thread. */
static void
on_terminate_notifier_read(h2o_socket_t *sock, const char *err)
{
	if (err != NULL) {
		fprintf(stderr, "pipe error: %s\n", err);
		abort();
	}

	h2o_buffer_consume(&sock->input, sock->input->size);
	thread_ctx_t *const thread_ctx = get_curr_thread_ctx();
	on_termination_notification(&thread_ctx->terminate_notifier);
}
#endif /* SUPPORT_THR_TERMINATION */
#endif /* USE_LIBUV */

#ifdef SUPPORT_THR_TERMINATION
/* Launched in HTTP server thread. */
static inline void
init_terminate_notifier(thread_ctx_t *thread_ctx)
{
#ifdef USE_LIBUV
	uv_async_init(&thread_ctx->loop, &thread_ctx->terminate_notifier,
		(uv_async_cb)on_termination_notification);
#else /* USE_LIBUV */
	h2o_loop_t *const loop = thread_ctx->ctx.loop;
	int fds[2];

	if (cloexec_pipe(fds) != 0) {
		perror("pipe");
		abort();
	}
	if (fcntl(fds[1], F_SETFL, O_NONBLOCK) != 0) {
		perror("fcntl");
		abort();
	}
	thread_ctx->terminate_notifier.write_fd = fds[1];
	thread_ctx->terminate_notifier.read_socket =
		h2o_evloop_socket_create(loop, fds[0], 0);
	h2o_socket_read_start(thread_ctx->terminate_notifier.read_socket,
		on_terminate_notifier_read);
#endif /* USE_LIBUV */
}
#endif /* SUPPORT_THR_TERMINATION */

#ifdef SUPPORT_THR_TERMINATION
/* Launched in HTTP server thread. */
static inline void
deinit_terminate_notifier(thread_ctx_t *thread_ctx)
{
#ifdef USE_LIBUV
	/* FIXME: Such call in h2o proper uses free()
	 * as callback - bug? */
	uv_close((uv_handle_t *)&thread_ctx->terminate_notifier, NULL);
#else /* USE_LIBUV */
	h2o_socket_read_stop(thread_ctx->terminate_notifier.read_socket);
	h2o_socket_close(thread_ctx->terminate_notifier.read_socket);
	close(thread_ctx->terminate_notifier.write_fd);
#endif /* USE_LIBUV */
}
#endif /* SUPPORT_THR_TERMINATION */

/* This is HTTP server thread main function. */
void *
worker_func(void *param)
{
	const unsigned thread_idx = (unsigned)(uintptr_t)param;
	thread_ctx_t *const thread_ctx = &conf.thread_ctxs[thread_idx];
	curr_thread_ctx = thread_ctx;
#ifdef USE_LIBUV
#ifdef INIT_CTX_IN_HTTP_THREAD
	h2o_context_init(&thread_ctx->ctx, &thread_ctx->loop,
		&thread_ctx->globalconf);
#endif /* INIT_CTX_IN_HTTP_THREAD */
#else /* USE_LIBUV */
#ifdef INIT_CTX_IN_HTTP_THREAD
	h2o_context_init(&thread_ctx->ctx, h2o_evloop_create(),
		&thread_ctx->globalconf);
#endif /* INIT_CTX_IN_HTTP_THREAD */
#endif /* USE_LIBUV */
#ifdef SUPPORT_THR_TERMINATION
	init_terminate_notifier(thread_ctx);
#endif /* SUPPORT_THR_TERMINATION */

#ifdef SUPPORT_THR_TERMINATION
	httpng_sem_post(&thread_ctx->can_be_terminated);
#endif /* SUPPORT_THR_TERMINATION */
#ifdef USE_LIBUV
#error "multilisten code doesn't support Libuv now"
	/* Process incoming connections/data and requests
	 * from TX thread. */
	uv_run(&thread_ctx->loop, UV_RUN_DEFAULT);

	assert(thread_ctx->shutdown_requested);

	/* FIXME: Need more than one. */
	listener_ctx_t *const listener_ctx = &thread_ctx->listener_ctxs[0];
	uv_read_stop((uv_stream_t *)&listener_ctx->uv_tcp_listener);
	uv_close((uv_handle_t *)&listener_ctx->uv_tcp_listener, NULL);

#ifdef SUPPORT_GRACEFUL_THR_TERMINATION
	const bool use_graceful_shutdown = thread_ctx->use_graceful_shutdown;
	prepare_worker_for_shutdown(thread_ctx, use_graceful_shutdown);
#else /* SUPPORT_GRACEFUL_THR_TERMINATION */
	prepare_worker_for_shutdown(thread_ctx);
#endif /* SUPPORT_GRACEFUL_THR_TERMINATION */

	/* Process remaining requests from TX thread. */
	uv_run(&thread_ctx->loop, UV_RUN_DEFAULT);
	assert(thread_ctx->tx_done_notification_received);
#ifdef SUPPORT_GRACEFUL_THR_TERMINATION
	handle_worker_shutdown(thread_ctx, use_graceful_shutdown);
#else /* SUPPORT_GRACEFUL_THR_TERMINATION */
	handle_worker_shutdown(thread_ctx);
#endif /* SUPPORT_GRACEFUL_THR_TERMINATION */
#else /* USE_LIBUV */
	thread_ctx->sock_from_tx =
		h2o_evloop_socket_create(thread_ctx->ctx.loop,
			xtm_queue_consumer_fd(thread_ctx->queue_from_tx),
			H2O_SOCKET_FLAG_DONT_READ);

	h2o_socket_read_start(thread_ctx->sock_from_tx, on_call_from_tx);
	thread_ctx->queue_from_tx_fd_consumed = true;
	listening_sockets_start_read(thread_ctx);
	h2o_evloop_t *loop = thread_ctx->ctx.loop;
#ifdef SUPPORT_THR_TERMINATION
	while (!thread_ctx->shutdown_requested)
#else /* SUPPORT_THR_TERMINATION */
	while (true)
#endif /* SUPPORT_THR_TERMINATION */
		h2o_evloop_run(loop, INT32_MAX);

#ifdef SUPPORT_THR_TERMINATION
	listening_sockets_stop_read(thread_ctx);
	close_listening_sockets(thread_ctx);

#ifdef SUPPORT_GRACEFUL_THR_TERMINATION
	const bool use_graceful_shutdown = thread_ctx->use_graceful_shutdown;
	prepare_worker_for_shutdown(thread_ctx, use_graceful_shutdown);
#else /* SUPPORT_GRACEFUL_THR_TERMINATION */
	prepare_worker_for_shutdown(thread_ctx);
#endif /* SUPPORT_GRACEFUL_THR_TERMINATION */

	/* Process remaining requests from TX thread. */
	while (!thread_ctx->tx_done_notification_received)
		h2o_evloop_run(loop, 1);
#ifdef SUPPORT_GRACEFUL_THR_TERMINATION
	handle_worker_shutdown(thread_ctx, use_graceful_shutdown);
#else /* SUPPORT_GRACEFUL_THR_TERMINATION */
	handle_worker_shutdown(thread_ctx);
#endif /* SUPPORT_GRACEFUL_THR_TERMINATION */

	h2o_socket_read_stop(thread_ctx->sock_from_tx);
	h2o_socket_close(thread_ctx->sock_from_tx);
#endif /* SUPPORT_THR_TERMINATION */
#endif /* USE_LIBUV */

#ifdef SUPPORT_THR_TERMINATION
#ifdef INIT_CTX_IN_HTTP_THREAD
	h2o_context_dispose(&thread_ctx->ctx);
#endif /* INIT_CTX_IN_HTTP_THREAD */

	deinit_terminate_notifier(thread_ctx);
	httpng_sem_destroy(&thread_ctx->can_be_terminated);

	thread_ctx->thread_finished = true;
	__sync_synchronize();
#endif /* SUPPORT_THR_TERMINATION */
	return NULL;
}

#ifdef SUPPORT_RECONFIG
/* Launched in HTTP server thread.
 * N. b.: It may never be launched if thread terminates. */
void
become_ungraceful(thread_ctx_t *thread_ctx)
{
	close_existing_connections(thread_ctx);
}
#endif /* SUPPORT_RECONFIG */

#ifdef SUPPORT_C_ROUTER
/* Launched in HTTP server thread. */
void *
get_router_data(shuttle_t *shuttle, unsigned *max_len)
{
	*max_len = conf.max_path_len_lua;
	lua_handler_state_t *const state =
		(lua_handler_state_t *)&shuttle->payload;
	return &state->un.req.buffer;
}
#endif /* SUPPORT_C_ROUTER */
