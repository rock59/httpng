#ifndef HTTPNG_PRIVATE_H
#define HTTPNG_PRIVATE_H

#include "httpng_config.h"
#include <h2o.h>

#include <xtm/src/xtm_api.h>
#include "httpng_sem.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#define DEFAULT_threads 1
#define DEFAULT_max_conn_per_thread (256 * 1024)
#define DEFAULT_max_shuttles_per_thread 4096
#define DEFAULT_shuttle_size 65536
#define DEFAULT_max_body_len (1024 * 1024)
#ifdef SUPPORT_GRACEFUL_THR_TERMINATION
#define DEFAULT_thread_termination_timeout 60
#endif /* SUPPORT_GRACEFUL_THR_TERMINATION */

/* Limits are quite relaxed for now. */
#define MIN_threads 1
#define MIN_max_conn_per_thread 1
#define MIN_max_shuttles_per_thread 1
#define MIN_shuttle_size (sizeof(shuttle_t) + sizeof(uintptr_t))
#define MIN_max_body_len 0

/* Limits are quite relaxed for now. */
#define MAX_threads 16 /* More than 4 is hardly useful (Lua). */
#define MAX_max_conn_per_thread (256 * 1024 * 1024)
#define MAX_max_shuttles_per_thread UINT_MAX
#define MAX_shuttle_size (16 * 1024 * 1024)
#define MAX_max_body_len LLONG_MAX

#ifdef SUPPORT_WEBSOCKETS
#define WS_CLIENT_KEY_LEN 24 /* Hardcoded in H2O. */
#endif /* SUPPORT_WEBSOCKETS */

#define LUA_QUERY_NONE UINT_MAX

#define STATIC_ASSERT(x, desc) do { \
		enum { \
			/* Will trigger zero division. */ \
			__static_assert_placeholder = 1 / !!(x), \
		}; \
	} while(0);

#define my_container_of(ptr, type, member) ({ \
	const typeof( ((type *)0)->member  ) *__mptr = \
		(typeof( &((type *)0)->member  ))(ptr); \
	(type *)( (char *)__mptr - offsetof(type,member)  );})

typedef struct st_h2o_websocket_conn_t h2o_websocket_conn_t;
typedef unsigned shuttle_count_t;
#ifdef SUPPORT_LISTEN
typedef struct st_sni_map {
	SSL_CTX **ssl_ctxs; /* Set of all ctxs for listener */
	size_t ssl_ctxs_capacity;
	size_t ssl_ctxs_size;

	struct {
		h2o_iovec_t hostname;
		SSL_CTX *ssl_ctx;
	} *sni_fields;
	size_t sni_fields_size;
	size_t sni_fields_capacity;
} sni_map_t;
typedef sni_map_t servername_callback_arg_t;
#endif /* SUPPORT_LISTEN */

typedef struct waiter {
	struct waiter *next;
	struct fiber *fiber;
} waiter_t;

typedef struct {
	h2o_handler_t super;
} lua_h2o_handler_t;

typedef struct {
	h2o_globalconf_t globalconf;
	h2o_context_t ctx;
	struct listener_ctx *listener_ctxs;
	struct xtm_queue *queue_to_tx;
	struct xtm_queue *queue_from_tx;
#ifdef SUPPORT_THR_TERMINATION
	struct fiber *fiber_to_wake_on_shutdown;
#endif /* SUPPORT_THR_TERMINATION */
	waiter_t *call_from_tx_waiter;
	h2o_hostconf_t *hostconf;
#ifndef USE_LIBUV
	/* For xtm; it is probably not socket underneath. */
	h2o_socket_t *sock_from_tx;
#endif /* USE_LIBUV */
#ifdef USE_LIBUV
	uv_loop_t loop;
	uv_poll_t uv_poll_from_tx;
#ifdef SUPPORT_THR_TERMINATION
	uv_async_t terminate_notifier;
#endif /* SUPPORT_THR_TERMINATION */
#else /* USE_LIBUV */
#ifdef SUPPORT_THR_TERMINATION
	struct {
		int write_fd;
		h2o_socket_t *read_socket; /* pipe underneath. */
	} terminate_notifier;
#endif /* SUPPORT_THR_TERMINATION */
#endif /* USE_LIBUV */
#ifdef USE_SHUTTLES_MUTEX
	pthread_mutex_t shuttles_mutex;
#endif /* USE_SHUTTLES_MUTEX */
#ifdef SUPPORT_CONN_LIST
	h2o_linklist_t accepted_sockets;
#endif /* SUPPORT_CONN_LIST */
#ifdef SUPPORT_THR_TERMINATION
	httpng_sem_t can_be_terminated;
#endif /* SUPPORT_THR_TERMINATION */
	shuttle_count_t shuttle_counter;
	unsigned num_connections;
	unsigned idx;
#ifdef SUPPORT_THR_TERMINATION
	unsigned active_lua_fibers;
#endif /* SUPPORT_THR_TERMINATION */
	unsigned listeners_created;
	pthread_t tid;
	bool push_from_tx_is_sleeping;
#ifdef SUPPORT_THR_TERMINATION
	bool shutdown_req_sent; /* TX -> HTTP(S) server thread. */
	bool shutdown_requested; /* Someone asked us to shut down thread. */
#ifdef SUPPORT_GRACEFUL_THR_TERMINATION
	bool use_graceful_shutdown;
	bool do_not_exit_tx_fiber;
#endif /* SUPPORT_GRACEFUL_THR_TERMINATION */
	bool should_notify_tx_done;
	bool tx_done_notification_received;
	bool tx_fiber_should_exit;
	bool tx_fiber_finished;
	volatile bool thread_finished;
#endif /* SUPPORT_THR_TERMINATION */
#ifndef USE_LIBUV
	bool queue_from_tx_fd_consumed;
#endif /* USE_LIBUV */
} thread_ctx_t;

struct anchor;
typedef struct shuttle {
	h2o_req_t *never_access_this_req_from_tx_thread;
	struct anchor *anchor;
	thread_ctx_t *thread_ctx;

	/* never_access_this_req_from_tx_thread can only be used
	 * if disposed is false. */
	char disposed;

#if 0 /* Should remove after correcting sample C handlers code. */
	/* For use by handlers, initialized to false for new shuttles. */
	char stopped;

	char unused[sizeof(void *) - 2 * sizeof(char)];
#else
	char unused[sizeof(void *) - 1 * sizeof(char)];
#endif /* 0 */

	char payload[];
} shuttle_t;

typedef void shuttle_func_t(shuttle_t *);
typedef struct anchor {
	shuttle_t *shuttle;

	/* Can be NULL; it should set shuttle->disposed to true. */
	shuttle_func_t *user_free_shuttle;
} anchor_t;

typedef struct listener_ctx {
	h2o_accept_ctx_t accept_ctx;
#ifdef USE_LIBUV
	uv_tcp_t uv_tcp_listener;
#else /* USE_LIBUV */
	h2o_socket_t *sock;
#endif /* USE_LIBUV */
	int fd;
} listener_ctx_t;

#ifdef SUPPORT_WEBSOCKETS
typedef struct {
	shuttle_t *parent_shuttle;
	unsigned payload_bytes;
	char payload[];
} recv_data_t;
#endif /* SUPPORT_WEBSOCKETS */

#ifdef SUPPORT_WEBSOCKETS
typedef void recv_data_func_t(recv_data_t *);
#endif /* SUPPORT_WEBSOCKETS */

typedef struct {
	const char *name;
	const char *value;
	unsigned name_len;
	unsigned value_len;
} http_header_entry_t;

typedef struct {
	size_t content_length;
	unsigned num_headers;
	unsigned http_code;
	http_header_entry_t headers[];
} lua_first_response_only_t;

typedef struct {
	h2o_generator_t generator;
	unsigned payload_len;
	const char *payload;
	bool is_last_send;
} lua_any_response_t;

typedef struct {
	lua_any_response_t any;
	lua_first_response_only_t first; /* Must be last member of struct. */
} lua_response_struct_t;

/* FIXME: Make it ushort and add sanity checks. */
typedef unsigned header_offset_t;

typedef struct {
	header_offset_t name_size;
	header_offset_t value_size;
} received_http_header_handle_t;

typedef struct {
#ifdef SUPPORT_SPLITTING_LARGE_BODY
	size_t offset_within_body; /* For use by HTTP server thread. */
#endif /* SUPPORT_SPLITTING_LARGE_BODY */
	int lua_handler_ref; /* Reference to user Lua handler. */
	unsigned router_data_len;
	unsigned path_len;
	unsigned authority_len;
	unsigned query_at;
	unsigned num_headers;
	unsigned body_len;
	unsigned char method_len;
#ifdef SUPPORT_WEBSOCKETS
	unsigned char ws_client_key_len;
#endif /* SUPPORT_WEBSOCKETS */
	unsigned char version_major;
	unsigned char version_minor;
#ifdef SUPPORT_REQ_INFO
	bool is_encrypted;
#endif /* SUPPORT_REQ_INFO */
#ifdef SUPPORT_SPLITTING_LARGE_BODY
	bool is_body_incomplete;
#endif /* SUPPORT_SPLITTING_LARGE_BODY */
	char method[7];
	char buffer[];
} lua_first_request_only_t;

typedef struct {
	struct fiber *fiber;
#ifdef SUPPORT_WEBSOCKETS
	struct fiber *recv_fiber; /* Fiber for WebSocket recv handler. */
	h2o_websocket_conn_t *ws_conn;
	recv_data_t *recv_data; /* For WebSocket recv. */
	struct fiber *tx_fiber; /* The one which services requests
				 * from "our" HTTP server thread. */
#endif /* SUPPORT_WEBSOCKETS */
	waiter_t *waiter; /* Changed by TX thread. */
#ifdef SUPPORT_REQ_INFO
	struct sockaddr_storage peer;
	struct sockaddr_storage ouraddr;
#endif /* SUPPORT_REQ_INFO */
	int lua_state_ref;
#ifdef SUPPORT_WEBSOCKETS
	int lua_recv_handler_ref;
	int lua_recv_state_ref;
#endif /* SUPPORT_WEBSOCKETS */
	bool fiber_done;
	bool sent_something;
	bool cancelled; /* Changed by TX thread. */
#ifdef SUPPORT_WEBSOCKETS
	bool ws_send_failed; /* FIXME: Accessed from TX and HTTP threads. */

	/* FIXME: It is changed by HTTP server thread w/o barriers
	 * but checked everywhere. */
	bool upgraded_to_websocket;
#endif /* SUPPORT_WEBSOCKETS */

#ifdef SUPPORT_WEBSOCKETS
	bool is_recv_fiber_waiting;
	bool is_recv_fiber_cancelled;
	bool in_recv_handler;
	char ws_client_key[WS_CLIENT_KEY_LEN];
#endif /* SUPPORT_WEBSOCKETS */
	union { /* Can use struct instead when debugging. */
		lua_first_request_only_t req;
		lua_response_struct_t resp;
	} un; /* Must be last member of struct. */
} lua_handler_state_t;

typedef void lua_handler_state_func_t(lua_handler_state_t *);
typedef void thread_ctx_func_t(thread_ctx_t *);

typedef struct {
	struct listener_cfg *listener_cfgs;
	thread_ctx_t *thread_ctxs;
	struct fiber *(tx_fiber_ptrs[MAX_threads]);
#ifdef SUPPORT_GRACEFUL_THR_TERMINATION
	struct fiber *reaper_fiber;
	struct fiber *fiber_to_wake_by_reaper_fiber;
	struct fiber *fiber_to_wake_on_reaping_done;
#endif /* SUPPORT_GRACEFUL_THR_TERMINATION */
	lua_h2o_handler_t *lua_handler;
#ifdef SUPPORT_LISTEN
	sni_map_t **sni_maps;
#endif /* SUPPORT_LISTEN */
#ifdef SUPPORT_C_ROUTER
	fill_router_data_t *fill_router_data;
#endif /* SUPPORT_C_ROUTER */
#ifdef SUPPORT_GRACEFUL_THR_TERMINATION
	double thread_termination_timeout;
	double thr_timeout_start;
#endif /* SUPPORT_GRACEFUL_THR_TERMINATION */
	uint64_t openssl_security_level;
	long min_tls_proto_version;
	shuttle_count_t max_shuttles_per_thread;
	unsigned shuttle_size;
#ifdef SUPPORT_WEBSOCKETS
	unsigned recv_data_size;
#endif /* SUPPORT_WEBSOCKETS */
	unsigned num_listeners;
#ifndef USE_LIBUV
	unsigned num_accepts;
#endif /* USE_LIBUV */
	unsigned max_conn_per_thread;
#ifdef SUPPORT_RECONFIG
	unsigned num_desired_threads;
#endif /* SUPPORT_RECONFIG */
	unsigned num_threads;
	unsigned max_headers_lua;
	unsigned max_path_len_lua;
#ifdef SUPPORT_WEBSOCKETS
	unsigned max_recv_bytes_lua_websocket;
#endif /* SUPPORT_WEBSOCKETS */
	int lua_handler_ref;
#ifdef SUPPORT_RECONFIG
	int new_lua_handler_ref;
#endif /* SUPPORT_RECONFIG */
#ifdef SUPPORT_ROUTER
	int router_ref;
#endif /* SUPPORT_ROUTER */
	int tfo_queues;
#ifdef SUPPORT_SHUTDOWN
	int on_shutdown_ref;
#endif /* SUPPORT_SHUTDOWN */
#ifdef SUPPORT_SHUTDOWN
	unsigned char reaping_flags;
#endif /* SUPPORT_SHUTDOWN */
#ifdef SUPPORT_SPLITTING_LARGE_BODY
	bool use_body_split;
#endif /* SUPPORT_SPLITTING_LARGE_BODY */
	bool configured;
	bool cfg_in_progress;
#ifdef SUPPORT_RECONFIG
	bool hot_reload_in_progress;
#endif /* SUPPORT_RECONFIG */
#ifdef SUPPORT_SHUTDOWN
	bool is_on_shutdown_setup;
	bool is_shutdown_in_progress;
#endif /* SUPPORT_SHUTDOWN */
#ifdef SUPPORT_GRACEFUL_THR_TERMINATION
	bool reaper_should_exit;
	bool reaper_exited;
	bool is_thr_term_timeout_active;
#endif /* SUPPORT_GRACEFUL_THR_TERMINATION */
#ifdef SUPPORT_SHUTDOWN
	bool inject_shutdown_error;
#endif /* SUPPORT_SHUTDOWN */
} conf_t;

extern conf_t conf;

extern __thread thread_ctx_t *curr_thread_ctx;

void cancel_processing_lua_req_in_tx(shuttle_t *);
void continue_processing_lua_req_in_tx(lua_handler_state_t *);
void free_shuttle_with_anchor(shuttle_t *);
void process_lua_req_in_tx(shuttle_t *);
void postprocess_lua_req_first(shuttle_t *);
void postprocess_lua_req_others(shuttle_t *);
void *worker_func(void *);
int lua_req_handler(lua_h2o_handler_t *, h2o_req_t *);
void close_listening_sockets(thread_ctx_t *);
#ifdef SUPPORT_WEBSOCKETS
void postprocess_lua_req_upgrade_to_websocket(shuttle_t *);
void postprocess_lua_req_websocket_send_text(lua_handler_state_t *);
void process_lua_websocket_received_data_in_tx(recv_data_t *);
void cancel_processing_lua_websocket_in_tx(lua_handler_state_t *);
void close_websocket(lua_handler_state_t *);
#endif /* SUPPORT_WEBSOCKETS */
#ifdef SUPPORT_SPLITTING_LARGE_BODY
void retrieve_more_body(shuttle_t *);
#endif /* SUPPORT_SPLITTING_LARGE_BODY */
#ifdef SUPPORT_THR_TERMINATION
void finish_processing_lua_reqs_in_tx(thread_ctx_t *);
void tx_done(thread_ctx_t *);
void tell_tx_fiber_to_exit(thread_ctx_t *);
#endif /* SUPPORT_THR_TERMINATION */
#ifdef SUPPORT_ROUTER
int router_wrapper(lua_h2o_handler_t *, h2o_req_t *);
#endif /* SUPPORT_ROUTER */
#ifdef SUPPORT_LISTEN
int servername_callback(SSL *, int *, void *);
#endif /* SUPPORT_LISTEN */
#ifdef SUPPORT_RECONFIG
void become_ungraceful(thread_ctx_t *);
#endif /* SUPPORT_RECONFIG */

/* FIXME: Maybe rename after changing sample C handlers? */
shuttle_t *prepare_shuttle2(h2o_req_t *);

/* Launched in HTTP server thread or in TX thread
 * when !SHOULD_FREE_SHUTTLE_IN_HTTP_SERVER_THREAD.
 * FIXME: Rename after changing sample C handlers. */
static inline void
free_shuttle(shuttle_t *shuttle)
{
	thread_ctx_t *const thread_ctx = shuttle->thread_ctx;
#ifdef USE_SHUTTLES_MUTEX
	pthread_mutex_lock(&thread_ctx->shuttles_mutex);
#endif /* USE_SHUTTLES_MUTEX */
	--thread_ctx->shuttle_counter;
#ifdef USE_SHUTTLES_MUTEX
	pthread_mutex_unlock(&thread_ctx->shuttles_mutex);
#endif /* USE_SHUTTLES_MUTEX */
	free(shuttle);
}

/* Can be launched in TX thread or HTTP(S) server thread. */
static inline shuttle_t *
get_shuttle(lua_handler_state_t *state)
{
	return (shuttle_t *)((char *)state - offsetof(shuttle_t, payload));
}

#ifdef SUPPORT_WEBSOCKETS
/* Can be launched in TX thread or HTTP server thread. */
static inline char *
get_websocket_recv_location(recv_data_t *const recv_data)
{
	return recv_data->payload;
}
#endif /* SUPPORT_WEBSOCKETS */

#ifdef __cplusplus
} /* extern "C" */
#endif /* __cplusplus */
#endif /* HTTPNG_PRIVATE_H */
