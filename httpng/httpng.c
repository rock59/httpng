#ifdef USE_LIBUV
#define H2O_USE_LIBUV 1
#else
#define H2O_USE_EPOLL 1 /* FIXME */
#include <h2o/evloop_socket.h>
#endif /* USE_LIBUV */
#include <h2o.h>

#ifdef SUPPORT_C_ROUTER
struct lua_State;
typedef void (fill_router_data_t)(struct lua_State *L, const char *path,
	unsigned data_len, void *data);
#define ROUTER_C_HANDLER_FIELD_NAME "_handle_request"
#define ROUTER_C_HANDLER_PARAM_FIELD_NAME "_handle_request_param"
#define ROUTER_FILL_ROUTER_DATA_FIELD_NAME "_fill_router_data"
#endif /* SUPPORT_C_ROUTER */

#include "process_helper.h"
#include <fcntl.h>
#include <float.h>
#include <poll.h>

#include <lauxlib.h>
#include <module.h>

#include <xtm/src/xtm_api.h>

#include <h2o/websocket.h>
#include <h2o/serverutil.h>
#include "../third_party/h2o/deps/cloexec/cloexec.h"
#include "httpng_sem.h"
#include <openssl/err.h>
#include "openssl_utils.h"

#ifdef USE_LIBUV
#include <uv.h>
#include <h2o/socket/uv-binding.h>
#endif /* USE_LIBUV */

#ifndef USE_LIBUV
/* evloop requires initing ctx in http thread (uses thread local vars).
 * Can't easily do the same for libuv - a lot of initializiation is
 * required and some functions can return errors.
 * */
#define INIT_CTX_IN_HTTP_THREAD
#endif /* USE_LIBUV */

#ifndef lengthof
#define lengthof(array) (sizeof(array) / sizeof((array)[0]))
#endif

#define STATIC_ASSERT(x, desc) do { \
		enum { \
			/* Will trigger zero division. */ \
			__static_assert_placeholder = 1 / !!(x), \
		}; \
	} while(0);

#ifdef TCP_FASTOPEN
#define H2O_DEFAULT_LENGTH_TCP_FASTOPEN_QUEUE 4096
#else
#define H2O_DEFAULT_LENGTH_TCP_FASTOPEN_QUEUE 0
#endif /* TCP_FASTOPEN */

/* Failing HTTP requests is fine, but failing to respond from TX thread
 * is not so queue size must be larger */
#define QUEUE_TO_TX_ITEMS (1 << 12) /* Must be power of 2 */
#define QUEUE_FROM_TX_ITEMS (QUEUE_TO_TX_ITEMS << 1) /* Must be power of 2 */

/* We would need this when (if) alloc would be performed from thread pools
 * w/o mutexes. */
#define SHOULD_FREE_SHUTTLE_IN_HTTP_SERVER_THREAD
#undef SHOULD_FREE_SHUTTLE_IN_HTTP_SERVER_THREAD

/* We would need this when (if) alloc would be performed from thread pools
 * w/o mutexes. */
#define SHOULD_FREE_RECV_DATA_IN_HTTP_SERVER_THREAD
#undef SHOULD_FREE_RECV_DATA_IN_HTTP_SERVER_THREAD

#ifndef SHOULD_FREE_SHUTTLE_IN_HTTP_SERVER_THREAD
#define USE_SHUTTLES_MUTEX
#endif /* SHOULD_FREE_SHUTTLE_IN_HTTP_SERVER_THREAD */

/* When disabled, HTTP requests with body not fitting into shuttle are failed.
 * N. b.: h2o allocates memory for the WHOLE body in any case. */
#define SUPPORT_SPLITTING_LARGE_BODY
//#undef SUPPORT_SPLITTING_LARGE_BODY

/* libh2o uses these magic values w/o defines. */
#define H2O_DEFAULT_PORT_FOR_PROTOCOL_USED 65535
#define H2O_CONTENT_LENGTH_UNSPECIFIED SIZE_MAX

#define LUA_QUERY_NONE UINT_MAX

#define WS_CLIENT_KEY_LEN 24 /* Hardcoded in H2O. */

#define DEFAULT_threads 1
#define DEFAULT_max_conn_per_thread (256 * 1024)
#define DEFAULT_max_shuttles_per_thread 4096
#define DEFAULT_shuttle_size 65536
#define DEFAULT_max_body_len (1024 * 1024)
#define DEFAULT_thread_termination_timeout 60

/* Limits are quite relaxed for now. */
#define MIN_threads 1
#define MIN_max_conn_per_thread 1
#define MIN_max_shuttles_per_thread 1
#define MIN_shuttle_size (sizeof(shuttle_t) + sizeof(uintptr_t))
#define MIN_max_body_len 0

/* Limits are quite relaxed for now. */
#define MAX_threads 16 /* More than 4 is hardly useful (Lua). */
#define MAX_max_conn_per_thread (256 * 1024 * 1024)
typedef unsigned shuttle_count_t;
#define MAX_max_shuttles_per_thread UINT_MAX
#define MAX_shuttle_size (16 * 1024 * 1024)
#define MAX_max_body_len LLONG_MAX

/* N.b.: for SSL3 to work you should probably use custom OpenSSL build. */
#define SSL3_STR "ssl3"
#define TLS1_STR "tls1"
#define TLS1_0_STR "tls1.0"
#define TLS1_1_STR "tls1.1"
#define TLS1_2_STR "tls1.2"
#define TLS1_3_STR "tls1.3"

#define GENERATION_INCREMENT 1

#define REAPING_GRACEFUL (1 << 0)
#define REAPING_UNGRACEFUL (1 << 1)

#define my_container_of(ptr, type, member) ({ \
	const typeof( ((type *)0)->member  ) *__mptr = \
		(typeof( &((type *)0)->member  ))(ptr); \
	(type *)( (char *)__mptr - offsetof(type,member)  );})

#define DEFAULT_MIN_TLS_PROTO_VERSION_NUM TLS1_2_VERSION
#define DEFAULT_MIN_TLS_PROTO_VERSION_STR TLS1_2_STR
#define DEFAULT_OPENSSL_SECURITY_LEVEL 1

#define STR_PORT_LENGTH 8

#define SUPPORT_GRACEFUL_THR_TERMINATION
//#undef SUPPORT_GRACEFUL_THR_TERMINATION
#define SUPPORT_RECONFIG
//#undef SUPPORT_RECONFIG

struct listener_ctx;

typedef struct {
#ifdef USE_LIBUV
	uv_tcp_t
#else /* USE_LIBUV */
	struct st_h2o_evloop_socket_t
#endif /* USE_LIBUV */
		super;
	h2o_linklist_t accepted_list;
} our_sock_t;

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
	struct fiber *fiber_to_wake_on_shutdown;
	waiter_t *call_from_tx_waiter;
	h2o_hostconf_t *hostconf;
#ifndef USE_LIBUV
	/* For xtm; it is probably not socket underneath. */
	h2o_socket_t *sock_from_tx;
#endif /* USE_LIBUV */
#ifdef USE_LIBUV
	uv_loop_t loop;
	uv_poll_t uv_poll_from_tx;
	uv_async_t terminate_notifier;
#else /* USE_LIBUV */
	struct {
		int write_fd;
		h2o_socket_t *read_socket; /* pipe underneath. */
	} terminate_notifier;
#endif /* USE_LIBUV */
#ifdef USE_SHUTTLES_MUTEX
	pthread_mutex_t shuttles_mutex;
#endif /* USE_SHUTTLES_MUTEX */
	h2o_linklist_t accepted_sockets;
	httpng_sem_t can_be_terminated;
	shuttle_count_t shuttle_counter;
	unsigned num_connections;
	unsigned idx;
	unsigned active_lua_fibers;
	unsigned listeners_created;
	pthread_t tid;
	bool push_from_tx_is_sleeping;
	bool xtm_queues_flushed;
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
#ifndef USE_LIBUV
	volatile bool queue_from_tx_fd_consumed;
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

/* Written directly into h2o_create_handler()->on_req. */
typedef int (*req_handler_t)(h2o_handler_t *, h2o_req_t *);

typedef int (*init_userdata_in_tx_t)(void *); /* Returns 0 on success. */

typedef struct {
	const char *path;
	req_handler_t handler;
	init_userdata_in_tx_t init_userdata_in_tx;
	void *init_userdata_in_tx_param;
} path_desc_t;

typedef struct listener_ctx {
	h2o_accept_ctx_t accept_ctx;
#ifdef USE_LIBUV
	uv_tcp_t uv_tcp_listener;
#else /* USE_LIBUV */
	h2o_socket_t *sock;
#endif /* USE_LIBUV */
	int fd;
} listener_ctx_t;

typedef struct {
	SSL_CTX *ssl_ctx;
	int fd;
	bool is_opened;
} listener_cfg_t;

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

typedef struct {
	shuttle_t *parent_shuttle;
	unsigned payload_bytes;
	char payload[];
} recv_data_t;

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
	unsigned char ws_client_key_len;
	unsigned char version_major;
	unsigned char version_minor;
	bool is_encrypted;
#ifdef SUPPORT_SPLITTING_LARGE_BODY
	bool is_body_incomplete;
#endif /* SUPPORT_SPLITTING_LARGE_BODY */
	char method[7];
	char buffer[];
} lua_first_request_only_t;

typedef struct {
	struct fiber *fiber;
	struct fiber *recv_fiber; /* Fiber for WebSocket recv handler. */
	h2o_websocket_conn_t *ws_conn;
	recv_data_t *recv_data; /* For WebSocket recv. */
	struct fiber *tx_fiber; /* The one which services requests
				 * from "our" HTTP server thread. */
	waiter_t *waiter; /* Changed by TX thread. */
	struct sockaddr_storage peer;
	struct sockaddr_storage ouraddr;
	int lua_state_ref;
	int lua_recv_handler_ref;
	int lua_recv_state_ref;
	bool fiber_done;
	bool sent_something;
	bool cancelled; /* Changed by TX thread. */
	bool ws_send_failed; /* FIXME: Accessed from TX and HTTP threads. */

	/* FIXME: It is changed by HTTP server thread w/o barriers
	 * but checked everywhere. */
	bool upgraded_to_websocket;

	bool is_recv_fiber_waiting;
	bool is_recv_fiber_cancelled;
	bool in_recv_handler;
	char ws_client_key[WS_CLIENT_KEY_LEN];
	union { /* Can use struct instead when debugging. */
		lua_first_request_only_t req;
		lua_response_struct_t resp;
	} un; /* Must be last member of struct. */
} lua_handler_state_t;

typedef void lua_handler_state_func_t(lua_handler_state_t *);
typedef void recv_data_func_t(recv_data_t *);
typedef void thread_ctx_func_t(thread_ctx_t *);
typedef int lua_handler_func_t(lua_h2o_handler_t *, h2o_req_t *);

static struct {
	listener_cfg_t *listener_cfgs;
	thread_ctx_t *thread_ctxs;
	struct fiber *(tx_fiber_ptrs[MAX_threads]);
#ifdef SUPPORT_GRACEFUL_THR_TERMINATION
	struct fiber *reaper_fiber;
	struct fiber *fiber_to_wake_by_reaper_fiber;
	struct fiber *fiber_to_wake_on_reaping_done;
#endif /* SUPPORT_GRACEFUL_THR_TERMINATION */
	lua_h2o_handler_t *lua_handler;
	sni_map_t **sni_maps;
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
	unsigned recv_data_size;
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
	unsigned max_recv_bytes_lua_websocket;
	int lua_handler_ref;
#ifdef SUPPORT_RECONFIG
	int new_lua_handler_ref;
#endif /* SUPPORT_RECONFIG */
	int router_ref;
	int tfo_queues;
	int on_shutdown_ref;
	unsigned char reaping_flags;
#ifdef SUPPORT_SPLITTING_LARGE_BODY
	bool use_body_split;
#endif /* SUPPORT_SPLITTING_LARGE_BODY */
	bool configured;
	bool cfg_in_progress;
#ifdef SUPPORT_RECONFIG
	bool hot_reload_in_progress;
#endif /* SUPPORT_RECONFIG */
	bool is_on_shutdown_setup;
	bool is_shutdown_in_progress;
	bool reaper_should_exit;
	bool reaper_exited;
#ifdef SUPPORT_GRACEFUL_THR_TERMINATION
	bool is_thr_term_timeout_active;
#endif /* SUPPORT_GRACEFUL_THR_TERMINATION */
	bool inject_shutdown_error;
} conf = {
	.tfo_queues = H2O_DEFAULT_LENGTH_TCP_FASTOPEN_QUEUE,
	.on_shutdown_ref = LUA_REFNIL,
	.max_shuttles_per_thread = DEFAULT_max_shuttles_per_thread,
};

__thread thread_ctx_t *curr_thread_ctx;

static struct sockaddr_un reaper_addr;
static uint32_t box_null_cdata_type;

static const char shuttle_field_name[] = "_shuttle";
#ifdef SUPPORT_RECONFIG
static const char msg_cant_reap[] =
	"Unable to reconfigure until threads will shut down";
static const char min_proto_version_reconf[] =
	"min_proto_version can't be changed on reconfiguration";
static const char openssl_security_level_reconf[] =
	"openssl_security_level can't be changed on reconfiguration";
#endif /* SUPPORT_RECONFIG */
static const char msg_bad_cert_num[] =
	"Only one key/certificate pair can be specified if SNI is disabled";
#ifndef NDEBUG
static const char msg_cant_switch_ssl_ctx[] =
	"Error while switching SSL context after scanning TLS SNI";
#endif /* NDEBUG */

/* FIXME: Rename after changing sample C handlers. */
extern void free_shuttle(shuttle_t *);

/* Must be called in HTTP server thread.
 * Should only be called if disposed==false.
 * Expected usage: when req handler can't or wouldn't queue request
 * to TX thread. */
extern void free_shuttle_with_anchor(struct shuttle *);

/* FIXME: Maybe rename after changing sample C handlers? */
extern shuttle_t *prepare_shuttle2(h2o_req_t *);

static void fill_http_headers(lua_State *L, lua_handler_state_t *state,
	int param_lua_idx);

#ifndef USE_LIBUV
static void on_terminate_notifier_read(h2o_socket_t *sock, const char *err);
#endif /* USE_LIBUV */
static void init_terminate_notifier(thread_ctx_t *thread_ctx);
static void deinit_terminate_notifier(thread_ctx_t *thread_ctx);
static int on_shutdown_callback(lua_State *L);

/* Launched in TX thread. */
static inline void
my_xtm_delete_queue_from_tx(thread_ctx_t *thread_ctx)
{
	xtm_queue_delete(thread_ctx->queue_from_tx,
		XTM_QUEUE_MUST_CLOSE_PRODUCER_READFD | (
#ifndef USE_LIBUV
		thread_ctx->queue_from_tx_fd_consumed ? 0 :
#endif /* USE_LIBUV */
		XTM_QUEUE_MUST_CLOSE_CONSUMER_READFD));
}

/* Launched in TX thread. */
__attribute__((weak)) void
complain_loudly_about_leaked_fds(void)
{
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

/* Launched in HTTP(S) server thread. */
static inline shuttle_t *
get_shuttle(lua_handler_state_t *state)
{
	return (shuttle_t *)((char *)state - offsetof(shuttle_t, payload));
}

/* Launched in TX thread.
 * FIXME: Use lua_tointegerx() when we would no longer care about
 * older Tarantool versions. */
static inline lua_Integer
my_lua_tointegerx(lua_State *L, int idx, int *ok)
{
	return (*ok = lua_isnumber(L, idx)) ? lua_tointeger(L, idx) : 0;
}

#ifdef SUPPORT_GRACEFUL_THR_TERMINATION
/* Launched in TX thread.
 * FIXME: Use lua_tonumberx() when we would no longer care about
 * older Tarantool versions. */
static inline lua_Number
my_lua_tonumberx(lua_State *L, int idx, int *ok)
{
	return (*ok = lua_isnumber(L, idx)) ? lua_tonumber(L, idx) : 0;
}
#endif /* SUPPORT_GRACEFUL_THR_TERMINATION */

/* Launched in HTTP server thread. */
static inline lua_handler_state_t *
get_lua_handler_state(h2o_generator_t *generator)
{
	return container_of(generator,
		lua_handler_state_t, un.resp.any.generator);
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

/* Launched in HTTP(S) server thread. */
static inline void
call_in_tx_with_shuttle(shuttle_func_t *func, shuttle_t *shuttle)
{
	assert(shuttle->thread_ctx == get_curr_thread_ctx());
	call_from_http_thr(get_queue_to_tx(), func, shuttle);
}

/* Launched in TX thread. */
static inline void
call_in_http_thr_with_shuttle(shuttle_func_t *func, shuttle_t *shuttle)
{
	call_from_tx(shuttle->thread_ctx->queue_from_tx, func, shuttle,
		shuttle->thread_ctx);
}

/* Launched in TX thread. */
static inline void
call_in_http_thr_with_lua_handler_state(lua_handler_state_func_t *func,
	lua_handler_state_t *state)
{
	shuttle_t *const shuttle = my_container_of(state, shuttle_t, payload);
	call_from_tx(shuttle->thread_ctx->queue_from_tx, func, state,
		shuttle->thread_ctx);
}

/* Launched in HTTP(S) server thread. */
static inline void
call_in_tx_with_lua_handler_state(lua_handler_state_func_t *func,
	lua_handler_state_t *param)
{
	assert(get_shuttle(param)->thread_ctx == get_curr_thread_ctx());
	call_from_http_thr(get_queue_to_tx(), func, param);
}

#ifdef SHOULD_FREE_RECV_DATA_IN_HTTP_SERVER_THREAD
/* Launched in TX thread. */
static inline void
call_in_http_thr_with_recv_data(recv_data_func_t *func, recv_data_t *recv_data)
{
	call_from_tx(recv_data->parent_shuttle->thread_ctx->queue_from_tx,
		func, recv_data, thread_ctx);
}
#endif /* SHOULD_FREE_RECV_DATA_IN_HTTP_SERVER_THREAD */

/* Launched in HTTP(S) server thread. */
static inline void
call_in_tx_with_recv_data(recv_data_func_t *func, recv_data_t *recv_data)
{
	assert(recv_data->parent_shuttle->thread_ctx == get_curr_thread_ctx());
	call_from_http_thr(get_queue_to_tx(), func, recv_data);
}

/* Launched in HTTP server thread. */
static inline void
call_in_tx_with_thread_ctx(thread_ctx_func_t *func, thread_ctx_t *thread_ctx)
{
	assert(thread_ctx == get_curr_thread_ctx());
	call_from_http_thr(thread_ctx->queue_to_tx, func, thread_ctx);
}

/* Launched in TX thread. */
static inline void
call_in_http_thr_with_thread_ctx(thread_ctx_func_t *func,
	thread_ctx_t *thread_ctx)
{
	call_from_tx(thread_ctx->queue_from_tx, func, thread_ctx, thread_ctx);
}

/* Launched in HTTP server thread. */
static inline recv_data_t *
alloc_recv_data(void)
{
	/* FIXME: Use per-thread pools? */
	recv_data_t *const recv_data = (recv_data_t *)
		malloc(conf.recv_data_size);
	return recv_data;
}

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

/* Launched in HTTP server thread or in TX thread when
 * !SHOULD_FREE_SHUTTLE_IN_HTTP_SERVER_THREAD. */
static void
free_shuttle_internal(shuttle_t *shuttle)
{
	assert(shuttle->disposed);
	free_shuttle(shuttle);
}

/* Launched in HTTP server thread or in TX thread when
 * !SHOULD_FREE_SHUTTLE_IN_HTTP_SERVER_THREAD.
 * FIXME: Only assert is different, can optimize for release build. */
static void
free_lua_websocket_shuttle_internal(shuttle_t *shuttle)
{
	assert(!shuttle->disposed);
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
#ifdef SHOULD_FREE_SHUTTLE_IN_HTTP_SERVER_THREAD
	free_shuttle_from_tx_in_http_thr(shuttle);
#else /* SHOULD_FREE_SHUTTLE_IN_HTTP_SERVER_THREAD */
	free_shuttle_internal(shuttle);
#endif /* SHOULD_FREE_SHUTTLE_IN_HTTP_SERVER_THREAD */
}

#ifndef SHOULD_FREE_SHUTTLE_IN_HTTP_SERVER_THREAD
/* Launched in TX thread. */
static inline void
free_lua_shuttle_from_tx(shuttle_t *shuttle)
{
	assert(!((lua_handler_state_t *)&shuttle->payload)
		->upgraded_to_websocket);
	free_shuttle_from_tx(shuttle);
}
#endif /* SHOULD_FREE_SHUTTLE_IN_HTTP_SERVER_THREAD */

/* Launched in TX thread. */
static inline void
free_lua_shuttle_from_tx_in_http_thr(shuttle_t *shuttle)
{
	assert(!((lua_handler_state_t *)&shuttle->payload)
		->upgraded_to_websocket);
	free_shuttle_from_tx_in_http_thr(shuttle);
}

/* Launched in TX thread. */
static inline void
free_cancelled_lua_not_ws_shuttle_from_tx(shuttle_t *shuttle)
{
#ifdef SHOULD_FREE_SHUTTLE_IN_HTTP_SERVER_THREAD
	free_shuttle_from_tx_in_http_thr(shuttle);
#else /* SHOULD_FREE_SHUTTLE_IN_HTTP_SERVER_THREAD */
	lua_handler_state_t *const state =
		(lua_handler_state_t *)&shuttle->payload;
	assert(!state->upgraded_to_websocket);
	if (state->sent_something)
		/* FIXME: We may check that all send operations has already
		 * been executed and skip going into HTTP server thread
		 * but this would made code more complex and require barriers.
		 * It is doubtful this would significantly improve
		 * real-world performance. */
		free_lua_shuttle_from_tx_in_http_thr(shuttle);
	else
		free_lua_shuttle_from_tx(shuttle);
#endif /* SHOULD_FREE_SHUTTLE_IN_HTTP_SERVER_THREAD */
}

/* Launched in TX thread. */
static inline void
free_lua_websocket_shuttle_from_tx(shuttle_t *shuttle)
{
	lua_handler_state_t *const state =
		(lua_handler_state_t *)&shuttle->payload;
	assert(state->upgraded_to_websocket);
	if (state->recv_fiber != NULL) {
		state->is_recv_fiber_cancelled = true;
		assert(state->is_recv_fiber_waiting);
		fiber_wakeup(state->recv_fiber);
		fiber_yield();
		struct lua_State *const L = luaT_state();
		luaL_unref(L, LUA_REGISTRYINDEX,
			state->lua_recv_handler_ref);
		luaL_unref(L, LUA_REGISTRYINDEX, state->lua_recv_state_ref);
	}
#ifdef SHOULD_FREE_SHUTTLE_IN_HTTP_SERVER_THREAD
	call_in_http_thr_with_shuttle(free_lua_websocket_shuttle_internal,
		shuttle);
#else /* SHOULD_FREE_SHUTTLE_IN_HTTP_SERVER_THREAD */
	free_lua_websocket_shuttle_internal(shuttle);
#endif /* SHOULD_FREE_SHUTTLE_IN_HTTP_SERVER_THREAD */
}

/* Launched in TX thread. */
static inline void
free_cancelled_lua_shuttle_from_tx(lua_handler_state_t *state)
{
	shuttle_t *const shuttle = get_shuttle(state);
	if (state->upgraded_to_websocket)
		free_lua_websocket_shuttle_from_tx(shuttle);
	else
		free_cancelled_lua_not_ws_shuttle_from_tx(shuttle);
}

/* Launched in HTTP server thread or in TX thread when
 * !SHOULD_FREE_RECV_DATA_IN_HTTP_SERVER_THREAD. */
static void
free_lua_websocket_recv_data_internal(recv_data_t *recv_data)
{
	free(recv_data);
}

/* Launched in TX thread. */
static inline void
free_lua_websocket_recv_data_from_tx(recv_data_t *recv_data)
{
#ifdef SHOULD_FREE_RECV_DATA_IN_HTTP_SERVER_THREAD
	call_in_http_thr_with_recv_data(free_lua_websocket_recv_data_internal,
		recv_data);
#else /* SHOULD_FREE_RECV_DATA_IN_HTTP_SERVER_THREAD */
	free_lua_websocket_recv_data_internal(recv_data);
#endif /* SHOULD_FREE_RECV_DATA_IN_HTTP_SERVER_THREAD */
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
static void
cancel_processing_lua_req_in_tx(shuttle_t *shuttle)
{
	lua_handler_state_t *const state =
		(lua_handler_state_t *)&shuttle->payload;
	assert(!state->upgraded_to_websocket);

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
	} else {
		state->cancelled = true;
		; /* Fiber would clean up because we have set cancelled=true */
	}
}

/* Launched in HTTP server thread. */
static void
free_shuttle_lua(shuttle_t *shuttle)
{
	lua_handler_state_t *const state =
		(lua_handler_state_t *)(&shuttle->payload);
	if (!state->upgraded_to_websocket) {
		shuttle->disposed = true;
		call_in_tx_with_shuttle(cancel_processing_lua_req_in_tx,
			shuttle);
	}
}

/* Launched in TX thread. */
static void
continue_processing_lua_req_in_tx(lua_handler_state_t *state)
{
	assert(state->fiber != NULL);
	assert(!state->fiber_done);
	wakeup_waiter(state);
}

/* Launched in TX thread. */
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
send_lua(h2o_req_t *req, lua_handler_state_t *const state)
{
	h2o_iovec_t buf;
	buf.base = (char *)state->un.resp.any.payload;
	buf.len = state->un.resp.any.payload_len;
	h2o_send(req, &buf, 1, state->un.resp.any.is_last_send
		? H2O_SEND_STATE_FINAL : H2O_SEND_STATE_IN_PROGRESS);
}

/* Launched in HTTP server thread to postprocess first response
 * (with HTTP headers). */
static void
postprocess_lua_req_first(shuttle_t *shuttle)
{
	if (shuttle->disposed)
		return;
	lua_handler_state_t *const state =
		(lua_handler_state_t *)(&shuttle->payload);
	h2o_req_t *const req = shuttle->never_access_this_req_from_tx_thread;
	req->res.status = state->un.resp.first.http_code;
	req->res.reason = "OK"; /* FIXME: Customizable? */
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

/* Launched in TX thread. */
static inline void
call_in_http_thr_postprocess_lua_req_first(shuttle_t *shuttle)
{
	call_in_http_thr_with_shuttle(postprocess_lua_req_first, shuttle);
}

/* Launched in HTTP server thread to postprocess response (w/o HTTP headers) */
static void
postprocess_lua_req_others(shuttle_t *shuttle)
{
	lua_handler_state_t *const state =
		(lua_handler_state_t *)(&shuttle->payload);
	if (shuttle->disposed)
		return;
	h2o_req_t *const req = shuttle->never_access_this_req_from_tx_thread;
	send_lua(req, state);
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
wait_for_lua_shuttle_return(lua_handler_state_t *state)
{
	if (state->cancelled)
		return;

	/* Add us into head of waiting list. */
	waiter_t waiter = { .next = state->waiter, .fiber = fiber_self() };
	state->waiter = &waiter;
	fiber_yield();
	if (state->waiter != NULL)
		wakeup_waiter(state);
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
	/* Lua parameters: self, payload, is_last. */
	enum {
		LUA_STACK_IDX_SELF = 1,
		LUA_STACK_IDX_PAYLOAD = 2,
		LUA_STACK_REQUIRED_PARAMS_COUNT = LUA_STACK_IDX_PAYLOAD,
		LUA_STACK_IDX_IS_LAST = 3,
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
	if (state->cancelled) {
		/* Returning Lua true because connection has already
		 * been closed. */
		lua_pushboolean(L, true);
		return 1;
	}

	size_t payload_len;
	state->un.resp.any.payload =
		lua_tolstring(L, LUA_STACK_IDX_PAYLOAD, &payload_len);
	state->un.resp.any.payload_len = payload_len;

	bool is_last;
	if (num_params >= LUA_STACK_IDX_IS_LAST)
		is_last	= lua_toboolean(L, LUA_STACK_IDX_IS_LAST);
	else
		is_last = false;

	state->un.resp.any.is_last_send = is_last;
	if (!state->sent_something) {
		state->un.resp.first.http_code =
			get_default_http_code(state);

		lua_getfield(L, LUA_STACK_IDX_SELF, "headers");
		const unsigned headers_lua_index = num_params + 1 + 1;
		fill_http_headers(L, state, headers_lua_index);

		state->sent_something = true;
		call_in_http_thr_postprocess_lua_req_first(shuttle);
	} else
		call_in_http_thr_postprocess_lua_req_others(shuttle);
	wait_for_lua_shuttle_return(state);

	/* Returning Lua true if connection has already been closed. */
	lua_pushboolean(L, state->cancelled);
	return 1;
}

/* Launched in TX thread. */
static void
fill_http_headers(lua_State *L, lua_handler_state_t *state, int param_lua_idx)
{
	state->un.resp.first.content_length =
		H2O_CONTENT_LENGTH_UNSPECIFIED;
	if (lua_isnil(L, param_lua_idx))
		return;

	lua_pushnil(L); /* Start of table. */
	while (lua_next(L, param_lua_idx)) {
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
			add_http_header_to_lua_response(
				&state->un.resp.first,
				key, key_len, value, value_len);

		/* Remove value, keep key for next iteration. */
		lua_pop(L, 1);
	}
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
	/* Lua parameters: self, code, headers, payload, is_last. */
	enum {
		LUA_STACK_IDX_SELF = 1,
		LUA_STACK_IDX_CODE = 2,
		LUA_STACK_REQUIRED_PARAMS_COUNT = LUA_STACK_IDX_CODE,
		LUA_STACK_IDX_HEADERS = 3,
		LUA_STACK_IDX_PAYLOAD = 4,
		LUA_STACK_IDX_IS_LAST = 5,
	};
	const unsigned num_params = lua_gettop(L);
	if (num_params < LUA_STACK_REQUIRED_PARAMS_COUNT)
		return luaL_error(L, "Not enough parameters");

	lua_getfield(L, LUA_STACK_IDX_SELF, shuttle_field_name);

	if (!lua_islightuserdata(L, -1))
		return luaL_error(L, "shuttle is invalid");
	shuttle_t *const shuttle = (shuttle_t *)lua_touserdata(L, -1);

	bool is_last;
	if (num_params >= LUA_STACK_IDX_IS_LAST)
		is_last	= lua_toboolean(L, LUA_STACK_IDX_IS_LAST);
	else
		is_last = false;

	lua_handler_state_t *const state =
		(lua_handler_state_t *)&shuttle->payload;
	take_shuttle_ownership_lua(state);
	if (state->sent_something)
		return luaL_error(L, "Handler has already written header");
	if (state->cancelled) {
		/* Can't send anything, connection has been closed.
		 * Returning Lua true because connection has already
		 * been closed. */
		lua_pushboolean(L, true);
		return 1;
	}

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
	fill_http_headers(L, state, headers_lua_index);

	if (num_params >= LUA_STACK_IDX_PAYLOAD) {
		size_t payload_len;
		state->un.resp.any.payload =
			lua_tolstring(L, LUA_STACK_IDX_PAYLOAD, &payload_len);
		state->un.resp.any.payload_len = payload_len;
	} else
		set_no_payload(state);

	state->un.resp.any.is_last_send = is_last;
	state->sent_something = true;
	call_in_http_thr_postprocess_lua_req_first(shuttle);
	wait_for_lua_shuttle_return(state);

	/* Returning Lua true if connection has already been closed. */
	lua_pushboolean(L, state->cancelled);
	return 1;
}

/* Launched in TX thread. */
static void
cancel_processing_lua_websocket_in_tx(lua_handler_state_t *state)
{
	assert(state->fiber != NULL);
	assert(!state->fiber_done);
	state->cancelled = true;
}

/* Can be launched in TX thread or HTTP server thread. */
static inline char *
get_websocket_recv_location(recv_data_t *const recv_data)
{
	return recv_data->payload;
}

/* Launched in TX thread. */
static inline const char *
get_router_entry_id(const lua_handler_state_t *state)
{
	/* FIXME: Query router for actual entry like "/foo". */
	(void)state;
	return "<unknown>";
}

/* Launched in TX thread. */
static void
process_lua_websocket_received_data_in_tx(recv_data_t *recv_data)
{
	shuttle_t *const shuttle = recv_data->parent_shuttle;
	assert(shuttle != NULL);
	lua_handler_state_t *const state =
		(lua_handler_state_t *)&shuttle->payload;
	assert(state->fiber != NULL);
	assert(!state->fiber_done);

	/* FIXME: Should we do this check in HTTP server thread? */
	if (state->recv_fiber != NULL) {
		if (state->is_recv_fiber_waiting) {
			state->recv_data = recv_data;
			fiber_wakeup(state->recv_fiber);
			fiber_yield();
		} else
			fprintf(stderr, "User WebSocket recv handler for "
				"\"\%s\" is NOT allowed to yield, data has "
				"been lost\n", get_router_entry_id(state));
	} else
		free_lua_websocket_recv_data_from_tx(recv_data);
}

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

/* Launched in HTTP server thread to postprocess upgrade to WebSocket. */
static void
postprocess_lua_req_upgrade_to_websocket(shuttle_t *shuttle)
{
	lua_handler_state_t *const state =
		(lua_handler_state_t *)(&shuttle->payload);
	if (shuttle->disposed)
		return;
	h2o_req_t *const req = shuttle->never_access_this_req_from_tx_thread;

	const unsigned num_headers = state->un.resp.first.num_headers;
	unsigned header_idx;
	for (header_idx = 0; header_idx < num_headers; ++header_idx) {
		const http_header_entry_t *const header =
			&state->un.resp.first.headers[header_idx];
		h2o_add_header_by_str(&req->pool, &req->res.headers,
			header->name, header->name_len,
			1, /* FIXME: Benchmark whether this faster than 0. */
			NULL, /* FIXME: Do we need orig_name? */
			header->value, header->value_len);

	}
	state->upgraded_to_websocket = true;
	state->ws_conn = h2o_upgrade_to_websocket(req,
		state->ws_client_key, shuttle, websocket_msg_callback);
	/* anchor_dispose()/free_shuttle_lua() will be called by h2o. */
	call_in_tx_continue_processing_lua_req(state);
}

/* Launched in HTTP server thread. */
static void
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

/* Launched in TX thread. */
static int
perform_ws_send_text(lua_State *L)
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
	if (state->in_recv_handler) {
		return luaL_error(L, "User WebSocket recv handler for "
			"\"%s\" is NOT allowed to call yielding functions",
			get_router_entry_id(state));
	}
	take_shuttle_ownership_lua(state);
	if (state->cancelled || state->ws_send_failed) {
		/* Returning Lua true because connection has already
		 * been closed or previous send failed. */
		lua_pushboolean(L, true);
		return 1;
	}

	size_t payload_len;
	state->un.resp.any.payload =
		lua_tolstring(L, LUA_STACK_IDX_PAYLOAD, &payload_len);
	state->un.resp.any.payload_len = payload_len;

	call_in_http_thr_with_lua_handler_state(
		postprocess_lua_req_websocket_send_text, state);
	wait_for_lua_shuttle_return(state);

	/* Returning Lua true if send failed or connection has already
	 * been closed. */
	lua_pushboolean(L, state->ws_send_failed || state->cancelled);
	return 1;
}

/* Launched in HTTP server thread. */
static void
close_websocket(lua_handler_state_t *const state)
{
	if (state->ws_conn != NULL) {
		h2o_websocket_close(state->ws_conn);
		state->ws_conn = NULL;
	}
	call_in_tx_continue_processing_lua_req(state);
}

/* Launched in TX thread. */
static inline void
call_in_http_thr_close_websocket(lua_handler_state_t *state)
{
	call_in_http_thr_with_lua_handler_state(close_websocket, state);
}

/* Launched in TX thread. */
static int
perform_ws_close(lua_State *L)
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

	lua_handler_state_t *const state =
		(lua_handler_state_t *)&shuttle->payload;
	if (state->in_recv_handler)
		return luaL_error(L, "User WebSocket recv handler for "
			"\"%s\" is NOT allowed to call yielding functions",
			get_router_entry_id(state));
	take_shuttle_ownership_lua(state);
	if (state->cancelled)
		return 0;

	state->cancelled = true;
	call_in_http_thr_close_websocket(state);
	wait_for_lua_shuttle_return(state);
	return 0;
}

/* Launched in TX thread. */
static int
lua_websocket_recv_fiber_func(va_list ap)
{
	shuttle_t *const shuttle = va_arg(ap, shuttle_t *);
	lua_State *const L = va_arg(ap, lua_State *);
	lua_handler_state_t *const state =
		(lua_handler_state_t *)&shuttle->payload;

	while (1) {
		state->is_recv_fiber_waiting = true;
		fiber_yield();
		if (state->is_recv_fiber_cancelled) {
			/* FIXME: Can we leak recv_data? */
			fiber_wakeup(state->fiber);
			return 0;
		}
		state->is_recv_fiber_waiting = false;

		/* User handler function, written in Lua. */
		lua_rawgeti(L, LUA_REGISTRYINDEX,
			state->lua_recv_handler_ref);

		recv_data_t *const recv_data = state->recv_data;
		assert(recv_data->parent_shuttle == shuttle);
		/* First param for Lua WebSocket recv handler - data. */
		lua_pushlstring(L, get_websocket_recv_location(recv_data),
			recv_data->payload_bytes);

		/* N. b.: WebSocket recv handler is NOT allowed to yield. */
		state->in_recv_handler = true;
		if (lua_pcall(L, 1, 0, 0) != LUA_OK)
			/* FIXME: Should probably log this instead(?).
			 * Should we stop calling handler? */
			fprintf(stderr, "User WebSocket recv handler for "
				"\"\%s\" failed with error \"%s\"\n",
				get_router_entry_id(state), lua_tostring(L, -1));
		state->in_recv_handler = false;
		free_lua_websocket_recv_data_from_tx(recv_data);
		fiber_wakeup(state->tx_fiber);
	}

	return 0;
}

/* Launched in TX thread. */
static int
perform_upgrade_to_websocket(lua_State *L)
{
	/* Lua parameters: self, headers, recv_function. */
	enum {
		LUA_STACK_IDX_SELF = 1,
		LUA_STACK_REQUIRED_PARAMS_COUNT = LUA_STACK_IDX_SELF,
		LUA_STACK_IDX_HEADERS = 2,
		LUA_STACK_IDX_RECV_FUNC = 3,
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
	if (state->sent_something)
		return luaL_error(L, "Unable to upgrade to WebSockets "
			"after sending HTTP headers");
	if (state->cancelled)
		/* Can't send anything, connection has been closed. */
		return 0;

	if (num_params >= LUA_STACK_IDX_HEADERS &&
	    !lua_isnil(L, LUA_STACK_IDX_HEADERS)) {
		lua_pushnil(L); /* Start of table. */
		while (lua_next(L, LUA_STACK_IDX_HEADERS)) {
			size_t key_len;
			size_t value_len;
			const char *const key = lua_tolstring(L, -2, &key_len);
			const char *const value =
				lua_tolstring(L, -1, &value_len);

			add_http_header_to_lua_response(
					&state->un.resp.first,
					key, key_len, value, value_len);

			/* Remove value, keep key for next iteration. */
			lua_pop(L, 1);
		}
	}

	if (num_params != LUA_STACK_IDX_RECV_FUNC ||
	    lua_type(L, LUA_STACK_IDX_RECV_FUNC) != LUA_TFUNCTION)
		state->recv_fiber = NULL;
	else {
		lua_pop(L, 1);
		state->lua_recv_handler_ref =
			luaL_ref(L, LUA_REGISTRYINDEX);
		struct lua_State *const new_L = lua_newthread(L);
		state->lua_recv_state_ref = luaL_ref(L, LUA_REGISTRYINDEX);
		if ((state->recv_fiber =
		    fiber_new("HTTP Lua WebSocket recv fiber",
			    &lua_websocket_recv_fiber_func)) == NULL) {
			luaL_unref(L, LUA_REGISTRYINDEX,
				state->lua_recv_handler_ref);
			luaL_unref(L, LUA_REGISTRYINDEX,
				state->lua_recv_state_ref);
			lua_pushnil(L);
			return 1;
		}
		state->is_recv_fiber_waiting = false;
		state->is_recv_fiber_cancelled = false;
		fiber_start(state->recv_fiber, shuttle, new_L);
	}

	state->sent_something = true;
	state->ws_send_failed = false;
	state->in_recv_handler = false;
	call_in_http_thr_with_shuttle(postprocess_lua_req_upgrade_to_websocket,
		shuttle);
	wait_for_lua_shuttle_return(state);

	if (state->cancelled)
		return 0;

	lua_createtable(L, 0, 3);
	lua_pushcfunction(L, perform_ws_send_text);
	lua_setfield(L, -2, "send_text");
	lua_pushcfunction(L, perform_ws_close);
	lua_setfield(L, -2, "close");
	lua_pushlightuserdata(L, shuttle);
	lua_setfield(L, -2, shuttle_field_name);
	return 1;
}

#ifdef SUPPORT_SPLITTING_LARGE_BODY
/* Launched in HTTP server thread. */
static void
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

#ifdef SUPPORT_C_ROUTER
/* Launched in TX thread. */
static inline void
fill_router_data(lua_State *L, const char *path, unsigned data_len, void *data)
{
	if (conf.fill_router_data != NULL)
		conf.fill_router_data(L, path, data_len, data);
}
#endif /* SUPPORT_C_ROUTER */

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
#ifdef SUPPORT_C_ROUTER
	fill_router_data(L, &state->un.req.buffer[
			state->un.req.router_data_len],
		state->un.req.router_data_len,
		&state->un.req.buffer);
#endif /* SUPPORT_C_ROUTER */
#ifdef SUPPORT_SPLITTING_LARGE_BODY
	if (!state->un.req.is_body_incomplete)
#endif /* SUPPORT_SPLITTING_LARGE_BODY */
	{
		lua_pushlstring(L, &state->un.req.buffer[current_offset],
			state->un.req.body_len);
		lua_setfield(L, -2, "body");
		return 0;
	}

#ifdef SUPPORT_SPLITTING_LARGE_BODY
	/* FIXME: Should use content-length to preallocate enough memory and
	 * avoid allocations and copying. Or we can just allocate in
	 * HTTP server thread and pass pointer. */
	char *body_buf = (char *)malloc(state->un.req.body_len);
	if (body_buf == NULL)
		/* There was memory allocation failure.
		 * FIXME: Should log this. */
		return 1;

	memcpy(body_buf, &state->un.req.buffer[current_offset],
		state->un.req.body_len);

	size_t body_offset = state->un.req.body_len;
	do {
		/* FIXME: Not needed on first iteration. */
		take_shuttle_ownership_lua(state);
		if (state->cancelled) {
			free(body_buf);
			return 1;
		}

		call_in_http_thr_with_shuttle(retrieve_more_body, shuttle);
		wait_for_lua_shuttle_return(state);
		if (state->cancelled) {
			free(body_buf);
			return 1;
		}

		{
			char *const new_body_buf = (char *)realloc(body_buf,
				body_offset + state->un.req.body_len);
			if (new_body_buf == NULL) {
				free(body_buf);
				/* There was memory allocation failure.
				 * FIXME: Should log this. */
				return 1;
			}
			body_buf = new_body_buf;
		}
		memcpy(&body_buf[body_offset], &state->un.req.buffer,
			state->un.req.body_len);
		body_offset += state->un.req.body_len;
	} while (state->un.req.is_body_incomplete);

	lua_pushlstring(L, body_buf, body_offset);
	free(body_buf);
	lua_setfield(L, -2, "body");
	return 0;
#endif /* SUPPORT_SPLITTING_LARGE_BODY */
}

/* Launched in TX thread. */
static void
close_lua_req_internal(lua_State *L, shuttle_t *shuttle)
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
		state->un.resp.first.http_code =
			get_default_http_code(state);
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
	close_lua_req_internal(L, shuttle);
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
	assert(!state->upgraded_to_websocket);
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
	assert(!state->upgraded_to_websocket);
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
		fill_http_headers(L, state, lua_gettop(L));
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
static void
tell_tx_fiber_to_exit(thread_ctx_t *thread_ctx)
{
	thread_ctx->tx_fiber_should_exit = true;
}

/* Launched in HTTP server thread. */
static void
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

/* Launched in TX thread. */
static inline void
call_in_http_thr_tx_done(thread_ctx_t *thread_ctx)
{
	call_in_http_thr_with_thread_ctx(tx_done, thread_ctx);
}

/* Launched in TX thread. */
static void
push_addr_table(lua_State *L, lua_handler_state_t *state,
	const char *name, ptrdiff_t offset)
{
	const struct sockaddr_storage *const ss =
		(struct sockaddr_storage *)((char *)state + offset);
	char tmp[(INET6_ADDRSTRLEN > INET_ADDRSTRLEN
		? INET6_ADDRSTRLEN : INET_ADDRSTRLEN)];
	const void *addr;
	const char *family;
	size_t family_len;
	unsigned port;
	if (ss->ss_family == AF_INET) {
		static const char str_af_inet[] = "AF_INET";
		const struct sockaddr_in *const in = (struct sockaddr_in *)ss;
		addr = &in->sin_addr;
		port = ntohs(in->sin_port);
		family = str_af_inet;
		family_len = sizeof(str_af_inet) - 1;
	} else if (ss->ss_family == AF_INET6) {
		static const char str_af_inet6[] = "AF_INET6";
		const struct sockaddr_in6 *const in = (struct sockaddr_in6 *)ss;
		addr = &in->sin6_addr;
		port = ntohs(in->sin6_port);
		family = str_af_inet6;
		family_len = sizeof(str_af_inet6) - 1;
	} else
		return;
	if (inet_ntop(ss->ss_family, addr, tmp, sizeof(tmp)) == NULL)
		return;

	lua_createtable(L, 0, 5);
	const size_t addr_len = strlen(tmp);
	lua_pushlstring(L, tmp, addr_len);
	lua_setfield(L, -2, "host");
	lua_pushlstring(L, family, family_len);
	lua_setfield(L, -2, "family");
	lua_pushinteger(L, port);
	lua_setfield(L, -2, "port");

	/* FIXME: Revisit for Unix sockets and HTTP/3 which uses UDP. */
	lua_pushlstring(L, "tcp", 3);
	lua_setfield(L, -2, "protocol");
	lua_pushlstring(L, "SOCK_STREAM", 11);
	lua_setfield(L, -2, "type");
	lua_setfield(L, -2, name);
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
process_handler_success_not_ws_without_send(lua_State *L, shuttle_t *shuttle)
{
	lua_handler_state_t *const state =
		(lua_handler_state_t *)&shuttle->payload;
	assert(!state->upgraded_to_websocket);
	if (state->un.resp.any.is_last_send) {
	Done:
		state->fiber_done = true;
		/* cancel_processing_lua_req_in_tx() is not yet called,
		 * it would clean up because we have set fiber_done=true. */
		return;
	}

	close_lua_req_internal(L, shuttle);
	if (!state->cancelled)
		goto Done;

	free_cancelled_lua_not_ws_shuttle_from_tx(shuttle);
}

/* Launched in TX thread */
static int
router_stash(lua_State *L)
{
	/* Lua parameters: self, name, [new_val]. */
	enum {
		LUA_STACK_IDX_SELF = 1,
		LUA_STACK_IDX_NAME = 2,
		LUA_STACK_REQUIRED_PARAMS_COUNT = LUA_STACK_IDX_NAME,
	};
	const unsigned num_params = lua_gettop(L);
	if (num_params < LUA_STACK_REQUIRED_PARAMS_COUNT)
		return luaL_error(L, "Not enough parameters");

	lua_getfield(L, LUA_STACK_IDX_SELF, "_stashes");
	if (!lua_istable(L, -1))
		return luaL_error(L, "_stashes is not a table");

	/* Push self._stashes[name]. */
	lua_pushvalue(L, LUA_STACK_IDX_NAME);

	lua_gettable(L, -2);
	return 1;
}

/* Launched in TX thread. */
static inline void
handle_ws_free(lua_handler_state_t *state)
{
	take_shuttle_ownership_lua(state);
	if (!state->cancelled) {
		call_in_http_thr_close_websocket(state);
		wait_for_lua_shuttle_return(state);
	}
	free_lua_websocket_shuttle_from_tx(get_shuttle(state));
}

/* Launched in TX thread. */
static int
lua_fiber_func(va_list ap)
{
	shuttle_t *const shuttle = va_arg(ap, shuttle_t *);
	lua_State *const L = va_arg(ap, lua_State *);

	lua_handler_state_t *const state =
		(lua_handler_state_t *)&shuttle->payload;
	thread_ctx_t *const thread_ctx = shuttle->thread_ctx;

	/* User handler function, written in Lua. */
	lua_rawgeti(L, LUA_REGISTRYINDEX, conf.lua_handler_ref);

	/* First param for Lua handler - req. */
	lua_createtable(L, 0, 15);
	lua_pushcfunction(L, router_stash);
	lua_setfield(L, -2, "stash");
	lua_pushinteger(L, state->un.req.version_major);
	lua_setfield(L, -2, "version_major");
	lua_pushinteger(L, state->un.req.version_minor);
	lua_setfield(L, -2, "version_minor");
	push_path(L, state);
	lua_pushlstring(L, &state->un.req.buffer[
		state->un.req.router_data_len +
		state->un.req.path_len], state->un.req.authority_len);
	lua_setfield(L, -2, "host");
	push_query(L, state);
	lua_pushlstring(L, state->un.req.method,
		state->un.req.method_len);
	lua_setfield(L, -2, "method");
	lua_pushboolean(L, !!state->un.req.ws_client_key_len);
	lua_setfield(L, -2, "is_websocket");
	lua_pushlightuserdata(L, shuttle);
	lua_setfield(L, -2, shuttle_field_name);
	push_addr_table(L, state, "peer", offsetof(lua_handler_state_t, peer));
	push_addr_table(L, state, "ouraddr",
		offsetof(lua_handler_state_t, ouraddr));
	if (state->un.req.is_encrypted) {
		lua_pushboolean(L, true);
		lua_setfield(L, -2, "https");
	}
	if (conf.router_ref != LUA_REFNIL) {
		lua_rawgeti(L, LUA_REGISTRYINDEX, conf.router_ref);
		lua_setfield(L, -2, "_used_router");
	}

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
	lua_createtable(L, 0, 6);
	lua_pushcfunction(L, perform_write_header);
	lua_setfield(L, -2, "write_header");
	lua_pushcfunction(L, perform_write);
	lua_setfield(L, -2, "write");
	lua_pushcfunction(L, perform_upgrade_to_websocket);
	lua_setfield(L, -2, "upgrade_to_websocket");
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
			/* No point trying to send something, connection
			 * has already been closed. */
			free_cancelled_lua_shuttle_from_tx(state);
		else if (state->upgraded_to_websocket)
			handle_ws_free(state);
		else
			process_handler_failure_not_ws(shuttle);
	} else if (state->cancelled)
		free_cancelled_lua_shuttle_from_tx(state);
	else if (state->upgraded_to_websocket)
		handle_ws_free(state);
	else if (lua_isnil(L, -1))
		process_handler_success_not_ws_without_send(L, shuttle);
	else
		process_handler_success_not_ws_with_send(L, shuttle);

Done:
	luaL_unref(luaT_state(), LUA_REGISTRYINDEX, lua_state_ref);
	if (--thread_ctx->active_lua_fibers == 0 &&
	    thread_ctx->should_notify_tx_done)
		call_in_http_thr_tx_done(thread_ctx);

	return 0;
}

/* Launched in TX thread. */
static void
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
		state->un.resp.first.content_length = \
			sizeof(error_str) - 1; \
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
	state->tx_fiber = fiber_self();
	++shuttle->thread_ctx->active_lua_fibers;
	fiber_start(state->fiber, shuttle, new_L);
}
#undef RETURN_WITH_ERROR

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
	const char *ws_client_key;
	(void)h2o_is_websocket_handshake(req, &ws_client_key);
	if (ws_client_key == NULL)
		state->un.req.ws_client_key_len = 0;
	else {
		state->un.req.ws_client_key_len = WS_CLIENT_KEY_LEN;
		memcpy(state->ws_client_key, ws_client_key,
			state->un.req.ws_client_key_len);
	}

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
			state->un.req.offset_within_body =
				body_bytes_to_copy;
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
	state->upgraded_to_websocket = false;
	state->waiter = NULL;

	socklen_t socklen = req->conn->callbacks->get_peername(req->conn,
		(struct sockaddr *)&state->peer);
	(void)socklen;
	assert(socklen <= sizeof(state->peer));
	socklen = req->conn->callbacks->get_sockname(req->conn,
		(struct sockaddr *)&state->ouraddr);
	assert(socklen <= sizeof(state->ouraddr));
	state->un.req.is_encrypted = h2o_is_req_transport_encrypted(req);

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
static int
lua_req_handler(lua_h2o_handler_t *self, h2o_req_t *req)
{
	shuttle_t *const shuttle = prepare_shuttle2(req);
	if (shuttle != NULL)
		lua_req_handler_ex(req, shuttle, 0);
	return 0;
}

#ifdef SUPPORT_RECONFIG
/* Launched in TX thread. */
static inline bool
is_router_used(void)
{
	return conf.lua_handler->super.on_req !=
		(int (*)(h2o_handler_t *, h2o_req_t *))lua_req_handler;
}
#endif /* SUPPORT_RECONFIG */

/* Launched in TX thread. */
static h2o_pathconf_t *
register_handler(h2o_hostconf_t *hostconf,
	const char *path, int (*on_req)(h2o_handler_t *, h2o_req_t *))
{
	/* These functions never return NULL, dying instead */
	h2o_pathconf_t *pathconf = h2o_config_register_path(hostconf, path, 0);
	h2o_handler_t *handler =
		h2o_create_handler(pathconf, sizeof(*handler));
	handler->on_req = on_req;
	return pathconf;
}

/* Launched in TX thread. */
static h2o_pathconf_t *
register_complex_handler_part_two(h2o_hostconf_t *hostconf,
	lua_handler_func_t *c_handler,
	void *c_handler_param, int router_ref)
{
	/* These functions never return NULL, dying instead */
	/* FIXME: It is probably unsafe to call these functions
	 * not from corresponding HTTP(S) thread. */
	h2o_pathconf_t *pathconf =
		h2o_config_register_path(hostconf, "/", 0);
	lua_h2o_handler_t *handler = (lua_h2o_handler_t *)
		h2o_create_handler(pathconf, sizeof(*handler));
	handler->super.on_req =
		(int (*)(h2o_handler_t *, h2o_req_t *))c_handler;
	conf.router_ref = router_ref;
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
static h2o_pathconf_t *
register_router_part_two(h2o_hostconf_t *hostconf,
	lua_handler_func_t *handler,
	void *handler_param, int router_ref)
{
	return register_complex_handler_part_two(hostconf, handler,
		handler_param, router_ref);
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

/* Launched in HTTP server thread. */
static int
router_wrapper(lua_h2o_handler_t *self, h2o_req_t *req)
{
	shuttle_t *const shuttle = prepare_shuttle2(req);
	if (shuttle != NULL)
		lua_req_handler_ex(req, shuttle, 0);
	return 0;
}

/* Launched in TX thread.
 * Returns !0 on error. */
static int
register_lua_router(lua_State *L, int router_ref, const char **lerr)
{
#ifdef SUPPORT_C_ROUTER
	lua_getmetatable(L, -2);
#else /* SUPPORT_C_ROUTER */
	lua_getmetatable(L, -1);
#endif /* SUPPORT_C_ROUTER */
	if (lua_isnil(L, -1)) {
		*lerr = "router table has neither C nor Lua handler functions";
		return 1;
	}
	lua_getfield(L, -1, "__call");
	if (lua_type(L, -1) != LUA_TFUNCTION) {
		*lerr = "there is no valid __call metamethod in router table";
		return 1;
	}
#ifdef SUPPORT_C_ROUTER
	conf.fill_router_data = NULL;
#endif /* SUPPORT_C_ROUTER */

	conf.lua_handler_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	unsigned thread_idx;
	for (thread_idx = 0; thread_idx < MAX_threads; ++thread_idx)
		register_router_part_two(conf.thread_ctxs[thread_idx].hostconf,
			router_wrapper, NULL, router_ref);
	return 0;
}

/* Launched in TX thread.
 * Returns !0 on error. */
static int
register_router(lua_State *L, int router_ref, const char **lerr)
{
	lua_rawgeti(L, LUA_REGISTRYINDEX, router_ref);

#ifdef SUPPORT_C_ROUTER
	lua_getfield(L, -1, ROUTER_FILL_ROUTER_DATA_FIELD_NAME);
	if (lua_isnil(L, -1))
		return register_lua_router(L, router_ref, lerr);
	if (!lua_islightuserdata(L, -1)) {
		*lerr = "router." ROUTER_FILL_ROUTER_DATA_FIELD_NAME
			" is not userdata";
		lua_pop(L, 2);
		return 1;
	}
	conf.fill_router_data = (fill_router_data_t *)lua_touserdata(L, -1);

	lua_getfield(L, -2, ROUTER_C_HANDLER_FIELD_NAME);
	if (!lua_islightuserdata(L, -1)) {
		*lerr = "router." ROUTER_C_HANDLER_FIELD_NAME
			" is not userdata";
		lua_pop(L, 3);
		return 1;
	}
	lua_handler_func_t *const user_handler =
		(lua_handler_func_t *)lua_touserdata(L, -1);

	lua_getfield(L, -3, ROUTER_C_HANDLER_PARAM_FIELD_NAME);
	if (!lua_isuserdata(L, -1)) {
		*lerr = "router." ROUTER_C_HANDLER_PARAM_FIELD_NAME
			" is not userdata";
		lua_pop(L, 4);
		return 1;
	}
	void *const user_handler_param = lua_touserdata(L, -1);
	lua_pop(L, 4);

	unsigned thread_idx;
	for (thread_idx = 0; thread_idx < MAX_threads; ++thread_idx)
		register_router_part_two(conf.thread_ctxs[thread_idx]
			.hostconf, user_handler,
			user_handler_param, LUA_REFNIL);
	return 0;
#else /* SUPPORT_C_ROUTER */
	return register_lua_router(L, router_ref, lerr);
#endif /* SUPPORT_C_ROUTER */
}

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

/* Launched in HTTP server thread or in TX thread
 * when !SHOULD_FREE_SHUTTLE_IN_HTTP_SERVER_THREAD. */
void
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

/* Launched in HTTP server thread. */
void
free_shuttle_with_anchor(shuttle_t *shuttle)
{
	assert(!shuttle->disposed);
	shuttle->anchor->shuttle = NULL;
	free_shuttle(shuttle);
}

/* Launched in HTTP server thread. */
static void
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
	h2o_linklist_unlink_fast(&my_container_of(data,
		our_sock_t, super)->accepted_list);
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
		struct st_h2o_evloop_socket_t *const sock =
			h2o_evloop_socket_accept_ex(listener,
				sizeof(our_sock_t));
		if (sock == NULL)
			return;

		our_sock_t *const item =
			container_of(sock, our_sock_t, super);
		h2o_linklist_insert_fast(&thread_ctx->accepted_sockets,
			&item->accepted_list);

		++thread_ctx->num_connections;

		sock->super.on_close.cb = on_underlying_socket_free;
		sock->super.on_close.data = sock;

		h2o_accept(&listener_ctx->accept_ctx, &sock->super);
	} while (--remain);
}

#endif /* USE_LIBUV */

/* Can be launched in TX thread or HTTP server thread. */
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
	listener_cfg_t *const listener_cfg =
		&conf.listener_cfgs[listener_idx];
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
#ifdef USE_LIBUV
#error "prepare_listening_sockets() not implemented for libuv yet"
#else /* USE_LIBUV */
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
				/* FIXME: Should report. */
				return false;
		} else
			listener_ctx->fd = listener_cfg->fd;
		set_cloexec(listener_ctx->fd);
		thread_ctx->listeners_created++;
	}
	return true;
#endif /* USE_LIBUV */
}

/* Can be launched in TX thread or HTTP server thread. */
static void
close_listening_sockets(thread_ctx_t *thread_ctx)
{
#ifdef USE_LIBUV
#error "close_listening_sockets() not implemented for libuv yet"
#else /* USE_LIBUV */
	listener_ctx_t *const listener_ctxs =
			thread_ctx->listener_ctxs;

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
#endif /* USE_LIBUV */
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

/* Launched in HTTP server thread. */
static void
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

/* Launched in HTTP server thread. */
static void
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
		listener_ctx->sock = h2o_evloop_socket_create(
				thread_ctx->ctx.loop,
				listener_ctx->fd, H2O_SOCKET_FLAG_DONT_READ);
		listener_ctx->sock->data = listener_ctx;
		h2o_socket_read_start(listener_ctx->sock, on_accept);
	}
#endif /* USE_LIBUV */
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
	if (ai_family == AF_INET6 &&
	    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &ipv6_flag,
		sizeof(ipv6_flag)) != 0) {
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
			/* FIXME: report in log */
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
			/* FIXME: report in log. */
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
static sni_map_t *
sni_map_create(int certs_num, const char **lerr)
{
	sni_map_t *sni_map = (sni_map_t *)calloc(1, sizeof(*sni_map));
	if (sni_map == NULL) {
		*lerr = "sni map memory allocation failed";
		goto sni_map_alloc_fail;
	}

	sni_map->ssl_ctxs = (typeof(sni_map->ssl_ctxs))
		calloc(certs_num, sizeof(*sni_map->ssl_ctxs));
	if (sni_map->ssl_ctxs == NULL) {
		*lerr = "memory allocation failed for ssl_ctxs in sni map";
		goto ssl_ctxs_alloc_fail;
	}
	sni_map->ssl_ctxs_capacity = certs_num;
	sni_map->sni_fields = (typeof(sni_map->sni_fields))
		calloc(certs_num, sizeof(*sni_map->sni_fields));
	if (sni_map->sni_fields == NULL) {
		*lerr = "memory allocation failed for sni_fields in sni map";
		goto sni_fields_alloc_fail;
	}
	sni_map->sni_fields_capacity = certs_num;
	return sni_map;

sni_fields_alloc_fail:
	free(sni_map->sni_fields);
ssl_ctxs_alloc_fail:
	free(sni_map->ssl_ctxs);
sni_map_alloc_fail:
	assert(*lerr != NULL);
	return NULL;
}

/* Launched in TX thread. */
static int
sni_map_insert(sni_map_t *sni_map, const char *certificate_file,
	       const char *certificate_key_file, const char **lerr)
{
	if (sni_map == NULL) {
		*lerr = "pointer to sni map is NULL";
		goto error;
	}

	assert(sni_map->ssl_ctxs_size < sni_map->ssl_ctxs_capacity);
	assert(sni_map->sni_fields_size < sni_map->sni_fields_capacity);

	X509 *X509_cert = get_X509_from_certificate_path(certificate_file,
		lerr);
	if (X509_cert == NULL)
		goto error;

	const char *common_name = get_subject_common_name(X509_cert);
	if (common_name == NULL) {
		*lerr = "can't get common name";
		goto x509_common_name_fail;
	}

	SSL_CTX *ssl_ctx = make_ssl_ctx(certificate_file, certificate_key_file,
		conf.openssl_security_level, conf.min_tls_proto_version, lerr);
	if (ssl_ctx == NULL)
		goto make_ssl_ctx_fail;
	sni_map->ssl_ctxs[sni_map->ssl_ctxs_size++] = ssl_ctx;
	sni_map->sni_fields[sni_map->sni_fields_size].hostname.base =
		(char *)common_name;
	sni_map->sni_fields[sni_map->sni_fields_size].hostname.len =
		strlen(common_name);
	sni_map->sni_fields[sni_map->sni_fields_size++].ssl_ctx = ssl_ctx;

	X509_free(X509_cert);
	return 0;

make_ssl_ctx_fail:
	free((char *)common_name);
x509_common_name_fail:
	X509_free(X509_cert);
error:
	assert(*lerr != NULL);
	return -1;
}

/* Launched in TX thread. 
 * Frees ssl contexts, host names of sni map and itself. */
static void
sni_map_free(sni_map_t *sni_map)
{
	if (sni_map == NULL)
		return;
	for (size_t i = 0; i < sni_map->ssl_ctxs_size; ++i)
		SSL_CTX_free(sni_map->ssl_ctxs[i]);
	for (size_t i = 0; i < sni_map->sni_fields_size; ++i)
		free((char *)sni_map->sni_fields[i].hostname.base);
	free(sni_map);
}

/* Launched in TX thread. */
static void
conf_sni_map_cleanup(void)
{
	if (conf.sni_maps == NULL)
		return;
	for (size_t i = 0; i < conf.num_listeners; ++i)
		sni_map_free(conf.sni_maps[i]);
	free(conf.sni_maps);
}

#define GET_REQUIRED_LISTENER_FIELD(name, lua_ttype, convert_func_postfix) \
	do { \
		lua_getfield(L, -1, #name); \
		if (lua_isnil(L, -1)) { \
			*lerr = #name " is absent"; \
			lua_pop(L, 1); \
			goto required_field_fail; \
		} \
		if (lua_type(L, -1) != lua_ttype) { \
			*lerr = #name " isn't " #convert_func_postfix; \
			lua_pop(L, 1); \
			goto required_field_fail; \
		} \
		name = lua_to##convert_func_postfix(L, -1); \
		lua_pop(L, 1); \
	} while (0);

/* Launched in TX thread. */
static SSL_CTX *
get_ssl_ctx_not_uses_sni(lua_State *L, unsigned listener_idx,
			 const char **lerr)
{
	SSL_CTX *ssl_ctx = NULL;

	unsigned certs_num = lua_objlen(L, -1);
	if (certs_num != 1) {
		*lerr = msg_bad_cert_num;
		goto certs_num_fail;
	}
	conf.sni_maps[listener_idx] = NULL;

	const char *certificate_file = NULL;
	const char *certificate_key_file = NULL;

	lua_rawgeti(L, -1, 1);
	if (!lua_istable(L, -1)) {
		*lerr = "element of `tls` table isn't a table";
		lua_pop(L, 1);
		goto tls_pair_not_a_table;
	}
	GET_REQUIRED_LISTENER_FIELD(certificate_file, LUA_TSTRING, string);
	GET_REQUIRED_LISTENER_FIELD(certificate_key_file, LUA_TSTRING, string);
	lua_pop(L, 1);

	ssl_ctx = make_ssl_ctx(certificate_file, certificate_key_file,
		conf.openssl_security_level, conf.min_tls_proto_version, lerr);
	if (ssl_ctx == NULL)
		goto ssl_ctx_create_fail;

	return ssl_ctx;

ssl_ctx_create_fail:
tls_pair_not_a_table:
required_field_fail:
certs_num_fail:
	assert(*lerr != NULL);
	return NULL;
}

/* Launched in HTTP thread. */
static int
servername_callback(SSL *s, int *al, void *arg)
{
	assert(arg != NULL);
	sni_map_t *sni_map = (servername_callback_arg_t *)arg;

	const char *servername = SSL_get_servername(s,
		TLSEXT_NAMETYPE_host_name);
	if (servername == NULL) {
#ifndef NDEBUG
		/* FIXME: report to log. */
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
				/* FIXME: report to log. */
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

/* Launched in TX thread. */
static SSL_CTX *
get_ssl_ctx_uses_sni(lua_State *L, unsigned listener_idx, const char **lerr)
{
	SSL_CTX *ssl_ctx = NULL;
	unsigned certs_num = lua_objlen(L, -1);
	sni_map_t *sni_map = NULL;
	if ((sni_map = sni_map_create(certs_num, lerr)) == NULL)
		goto sni_map_init_fail;

	lua_pushnil(L); /* Start of table. */
	while (lua_next(L, -2)) {
		const char *certificate_file = NULL;
		const char *certificate_key_file = NULL;

		GET_REQUIRED_LISTENER_FIELD(certificate_file,
			LUA_TSTRING, string);
		GET_REQUIRED_LISTENER_FIELD(certificate_key_file,
			LUA_TSTRING, string);

		if (sni_map_insert(sni_map, certificate_file,
		    certificate_key_file, lerr) != 0) {
			lua_pop(L, 1);
			goto sni_map_insert_fail;
		}
		lua_pop(L, 1);
	}

	ssl_ctx = make_ssl_ctx(NULL, NULL, conf.openssl_security_level,
		conf.min_tls_proto_version, lerr);
	if (ssl_ctx == NULL)
		goto ssl_ctx_create_fail;
	SSL_CTX_set_tlsext_servername_callback(ssl_ctx, servername_callback);
	SSL_CTX_set_tlsext_servername_arg(ssl_ctx,
		(servername_callback_arg_t *)sni_map);
	conf.sni_maps[listener_idx] = sni_map;
	return ssl_ctx;

ssl_ctx_create_fail:
required_field_fail:
sni_map_insert_fail:
	sni_map_free(sni_map);
sni_map_init_fail:
	assert(*lerr != NULL);
	return NULL;
}

/* Launched in TX thread. */
static SSL_CTX *
get_tls_field_from_lua(lua_State *L, unsigned listener_idx,
		       bool uses_sni, const char **lerr)
{
	if (!lua_istable(L, -1)) {
		*lerr = "`tls` isn't a table";
		goto tls_not_a_table;
	}
	unsigned certs_num = lua_objlen(L, -1);
	if (!uses_sni && certs_num != 1) {
		*lerr = msg_bad_cert_num;
		goto wrong_cert_num_and_uses_sni;
	}
	return uses_sni ? get_ssl_ctx_uses_sni(L, listener_idx, lerr)
			: get_ssl_ctx_not_uses_sni(L, listener_idx, lerr);

wrong_cert_num_and_uses_sni:
tls_not_a_table:
	assert(*lerr != NULL);
	return NULL;
}

/* Launched in TX thread. */
static int
load_default_listen_params(const char **lerr)
{
	conf.num_listeners = 2;
	conf.sni_maps = NULL;
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
		register_listener_cfgs_socket(fd, NULL, 0);
	}

	{
		const char *const addr = "::";

		const int fd = open_listener(addr, port, lerr);
		if (fd < 0)
			goto Error;
		register_listener_cfgs_socket(fd, NULL, 1);
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

	lua_getfield(L, lua_stack_idx_table, "listen");
	int need_to_pop = 1;
	if (lua_isnil(L, -1)) {
		if (load_default_listen_params(lerr) == 0) {
			lua_pop(L, need_to_pop);
			return 0;
		}
		goto listen_invalid_type;
	}

	if (!lua_istable(L, -1)) {
		*lerr = "listen isn't table";
		goto listen_invalid_type;
	}
	conf.num_listeners = lua_objlen(L, -1);

	if ((conf.listener_cfgs =
	    (typeof(conf.listener_cfgs))calloc(conf.num_listeners,
	    sizeof(*conf.listener_cfgs))) == NULL) {
		*lerr = "allocation memory for listener_cfgs failed";
		goto listener_cfg_malloc_fail;
	}

	if ((conf.sni_maps = (typeof(conf.sni_maps))calloc(conf.num_listeners,
	    sizeof(*conf.sni_maps)))== NULL) {
		*lerr = "allocation memory for sni maps failed";
		goto sni_map_alloc_fail;
	}

	size_t listener_idx = 0;
	lua_pushnil(L); /* Start of table. */
	while (lua_next(L, -2)) {
		++need_to_pop;
		if (!lua_istable(L, -1)) {
			*lerr = "`listen` must be table of tables";
			goto failed_parsing_clean_listen_conf;
		}

		const char *addr;
		uint16_t port;

		GET_REQUIRED_LISTENER_FIELD(addr, LUA_TSTRING, string);
		GET_REQUIRED_LISTENER_FIELD(port, LUA_TNUMBER, integer);

		SSL_CTX *ssl_ctx = NULL;
		lua_getfield(L, -1, "tls");
		++need_to_pop;
		if (lua_isnil(L, -1))
			conf.sni_maps[listener_idx] = NULL;
		else if (lua_istable(L, -1)) {
			lua_pop(L, 1);
			--need_to_pop;

			bool uses_sni;
			GET_REQUIRED_LISTENER_FIELD(uses_sni,
				LUA_TBOOLEAN, boolean);
			lua_getfield(L, -1, "tls");
			++need_to_pop;

			if ((ssl_ctx = get_tls_field_from_lua(L, listener_idx,
			    uses_sni, lerr)) == NULL)
				goto failed_parsing_clean_listen_conf;
		} else {
			*lerr = "`tls` isn't table or nil";
			goto failed_parsing_clean_listen_conf;
		}
		/* Pop "tls" and "clean" listen cfg. */
		lua_pop(L, 2);
		need_to_pop -= 2;

		int fd = open_listener(addr, port, lerr);
		if (fd < 0)
			goto open_listener_fail;
		register_listener_cfgs_socket(fd, ssl_ctx, listener_idx);
		++listener_idx;
	}

	/* Pop listen table. */
	lua_pop(L, 1);
	assert(--need_to_pop == 0);
	return 0;

open_listener_fail:
required_field_fail:
failed_parsing_clean_listen_conf:
	conf_sni_map_cleanup();
	close_listener_cfgs_sockets();
	conf.sni_maps = NULL;
sni_map_alloc_fail:
	free(conf.listener_cfgs);
	conf.listener_cfgs = NULL;
listener_cfg_malloc_fail:
listen_invalid_type:
	/* Pop values pushed while executing current function. */
	lua_pop(L, need_to_pop);
	EVP_cleanup();
	ERR_free_strings();
	assert(*lerr != NULL);
	return 1;
}

#undef GET_REQUIRED_LISTENER_FIELD

/* Launched in TX thread. */
static void
reset_thread_ctx(unsigned idx)
{
	thread_ctx_t *const thread_ctx = &conf.thread_ctxs[idx];

	thread_ctx->call_from_tx_waiter = NULL;
	thread_ctx->push_from_tx_is_sleeping = false;
	thread_ctx->should_notify_tx_done = false;
	thread_ctx->tx_fiber_should_exit = false;
	thread_ctx->shutdown_req_sent = false;
	thread_ctx->shutdown_requested = false;
#ifdef SUPPORT_GRACEFUL_THR_TERMINATION
	thread_ctx->use_graceful_shutdown = true;
#endif /* SUPPORT_GRACEFUL_THR_TERMINATION */
	thread_ctx->tx_done_notification_received = false;
	thread_ctx->tx_fiber_finished = false;
	thread_ctx->thread_finished = false;
#ifndef USE_LIBUV
	thread_ctx->queue_from_tx_fd_consumed = false;
#endif /* USE_LIBUV */
}

/* Launched in TX thread. */
static void
destroy_h2o_context_and_loop(thread_ctx_t *thread_ctx)
{
#ifdef USE_LIBUV
	uv_loop_close(&thread_ctx->loop);
#ifndef INIT_CTX_IN_HTTP_THREAD
	h2o_context_dispose(&thread_ctx->ctx);
#endif /* INIT_CTX_IN_HTTP_THREAD */
#else /* USE_LIBUV */
#ifndef INIT_CTX_IN_HTTP_THREAD
	h2o_context_dispose(&thread_ctx->ctx);
#endif /* INIT_CTX_IN_HTTP_THREAD */
	h2o_evloop_destroy(thread_ctx->ctx.loop);
#endif /* USE_LIBUV */
}

/* Launched in TX thread.
 * Returns false in case of error. */
static bool
init_worker_thread(unsigned thread_idx)
{
#ifdef USE_LIBUV
	int fd_consumed = 0;
#endif /* USE_LIBUV */
	thread_ctx_t *const thread_ctx = &conf.thread_ctxs[thread_idx];

#ifdef USE_SHUTTLES_MUTEX
	if (pthread_mutex_init(&thread_ctx->shuttles_mutex, NULL) != 0)
		/* FIXME: Report. */
		goto mutex_init_failed;
#endif /* USE_SHUTTLES_MUTEX */

	if ((thread_ctx->queue_from_tx = xtm_queue_new(QUEUE_FROM_TX_ITEMS))
	    == NULL)
		/* FIXME: Report. */
		goto alloc_xtm_failed;

	if ((thread_ctx->listener_ctxs = (listener_ctx_t *)
	    malloc(conf.num_listeners * sizeof(listener_ctx_t))) == NULL)
		/* FIXME: Report. */
		goto alloc_ctxs_failed;

	thread_ctx->shuttle_counter = 0;
	memset(&thread_ctx->ctx, 0, sizeof(thread_ctx->ctx));
#ifdef USE_LIBUV
	uv_loop_init(&thread_ctx->loop);
#ifndef INIT_CTX_IN_HTTP_THREAD
	h2o_context_init(&thread_ctx->ctx, &thread_ctx->loop,
		&thread_ctx->globalconf);
#endif /* INIT_CTX_IN_HTTP_THREAD */
#else /* USE_LIBUV */
	/* Can't call h2o_context_init() here, this must be done
	 * from HTTP thread because it (indirectly)
	 * uses thread-local variables. */
#ifndef INIT_CTX_IN_HTTP_THREAD
	h2o_context_init(&thread_ctx->ctx, h2o_evloop_create(),
		&thread_ctx->globalconf);
#endif /* INIT_CTX_IN_HTTP_THREAD */
#endif /* USE_LIBUV */
	h2o_linklist_init_anchor(&thread_ctx->accepted_sockets);

	if (!prepare_listening_sockets(thread_ctx))
		goto prepare_listening_sockets_failed;

#ifdef USE_LIBUV
	if (uv_tcp_init(thread_ctx->ctx.loop, &listener_ctx->uv_tcp_listener))
		/* FIXME: Should report. */
		goto uv_tcp_init_failed;
	if (uv_tcp_open(&listener_ctx->uv_tcp_listener, listener_ctx->fd))
		/* FIXME: Should report. */
		goto uv_tcp_open_failed;
	fd_consumed = 1;
	listener_ctx->uv_tcp_listener.data = listener_ctx;
	if (uv_listen((uv_stream_t *)&listener_ctx->uv_tcp_listener,
	    SOMAXCONN, on_accept))
		/* FIXME: Should report. */
		goto uv_listen_failed;

	if (uv_poll_init(thread_ctx->ctx.loop, &thread_ctx->uv_poll_from_tx,
	    xtm_queue_consumer_fd(thread_ctx->queue_from_tx)))
		/* FIXME: Should report. */
		goto uv_poll_init_failed;
	if (uv_poll_start(&thread_ctx->uv_poll_from_tx, UV_READABLE,
	    on_call_from_tx))
		goto uv_poll_start_failed;
#endif /* USE_LIBUV */

	return true;

#ifdef USE_LIBUV
uv_poll_start_failed:
	uv_close((uv_handle_t *)&thread_ctx->uv_poll_from_tx, NULL);
uv_poll_init_failed:
uv_listen_failed:
uv_tcp_open_failed:
	uv_close((uv_handle_t *)&listener_ctx->uv_tcp_listener, NULL);

uv_tcp_init_failed:
	if (!fd_consumed && thread_idx)
		close(listener_ctx->fd);
#endif /* USE_LIBUV */

prepare_listening_sockets_failed:
	close_listening_sockets(thread_ctx);
	destroy_h2o_context_and_loop(thread_ctx);

	free(thread_ctx->listener_ctxs);
alloc_ctxs_failed:
	my_xtm_delete_queue_from_tx(thread_ctx);
alloc_xtm_failed:
#ifdef USE_SHUTTLES_MUTEX
	pthread_mutex_destroy(&thread_ctx->shuttles_mutex);
mutex_init_failed:
#endif /* USE_SHUTTLES_MUTEX */
	return false;
}

/* Launched in TX thread. */
static void
finish_processing_lua_reqs_in_tx(thread_ctx_t *thread_ctx)
{
	if (thread_ctx->active_lua_fibers == 0)
		call_in_http_thr_tx_done(thread_ctx);
	else
		thread_ctx->should_notify_tx_done = true;
}

/* Launched in HTTP(S) server thread. */
static inline void
call_in_tx_finish_processing_lua_reqs(thread_ctx_t *thread_ctx)
{
	call_in_tx_with_thread_ctx(finish_processing_lua_reqs_in_tx,
		thread_ctx);
}

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

/* Launched in HTTP server thread. */
static void
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

/* Launched in HTTP server thread. */
static void
prepare_worker_for_shutdown(thread_ctx_t *thread_ctx
#ifdef SUPPORT_GRACEFUL_THR_TERMINATION
	, bool use_graceful_shutdown
#endif /*  SUPPORT_GRACEFUL_THR_TERMINATION */
	)
{
	/* FIXME: If we want to send something through existing
	 * connections, should do it now (accepts are already
	 * blocked). */

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

/* Launched in HTTP server thread. */
static void
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

/* This is HTTP server thread main function. */
static void *
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
	init_terminate_notifier(thread_ctx);

	__sync_synchronize();
	httpng_sem_post(&thread_ctx->can_be_terminated);
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
	while (!thread_ctx->shutdown_requested)
		h2o_evloop_run(loop, INT32_MAX);

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
#endif /* USE_LIBUV */

#ifdef INIT_CTX_IN_HTTP_THREAD
	h2o_context_dispose(&thread_ctx->ctx);
#endif /* INIT_CTX_IN_HTTP_THREAD */

	deinit_terminate_notifier(thread_ctx);
	httpng_sem_destroy(&thread_ctx->can_be_terminated);

	thread_ctx->thread_finished = true;
	__sync_synchronize();
	return NULL;
}

/* Launched in TX thread. */
static void
deinit_worker_thread(unsigned thread_idx)
{
	thread_ctx_t *const thread_ctx = &conf.thread_ctxs[thread_idx];

#ifdef USE_LIBUV
#error "multilisten code doesn't support Libuv now"
	/* FIXME: Need more than one. */
	listener_ctx_t *const listener_ctx = &thread_ctx->listener_ctxs[0];

	uv_read_stop((uv_stream_t *)&listener_ctx->uv_tcp_listener);
	uv_poll_stop(&thread_ctx->uv_poll_from_tx);
	uv_close((uv_handle_t *)&thread_ctx->uv_poll_from_tx, NULL);
	uv_close((uv_handle_t *)&listener_ctx->uv_tcp_listener, NULL);
#else /* USE_LIBUV */
	h2o_evloop_t *const loop = thread_ctx->ctx.loop;
	h2o_evloop_run(loop, 0); /* To actually free memory. */
#endif /* USE_LIBUV */

	destroy_h2o_context_and_loop(thread_ctx);

	/* FIXME: Should flush these queues first. */
	my_xtm_delete_queue_from_tx(thread_ctx);
	free(thread_ctx->listener_ctxs);
	assert(thread_ctx->shuttle_counter == 0);
#ifdef USE_SHUTTLES_MUTEX
	pthread_mutex_destroy(&thread_ctx->shuttles_mutex);
#endif /* USE_SHUTTLES_MUTEX */
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
	/* thread_ctx->tx_fiber_should_exit is read non-atomically for
	 * performance reasons so it should be changed in this thread by
	 * queueing corresponding function call. */
	while (!thread_ctx->tx_fiber_should_exit)
		if (coio_wait(pipe_fd, COIO_READ, DBL_MAX) & COIO_READ)
			invoke_all_in_tx(queue_to_tx);
	thread_ctx->tx_fiber_finished = true;
	if (thread_ctx->fiber_to_wake_on_shutdown != NULL) {
		struct fiber *const fiber =
			thread_ctx->fiber_to_wake_on_shutdown;
		thread_ctx->fiber_to_wake_on_shutdown = NULL;
		fiber_wakeup(fiber);
	}
	return 0;
}

/* Launched in HTTP server thread. */
static void
on_termination_notification(void *param)
{
	thread_ctx_t *const thread_ctx =
		my_container_of(param, thread_ctx_t, terminate_notifier);
	thread_ctx->shutdown_requested = true;
#ifdef USE_LIBUV
	uv_stop(&thread_ctx->loop);
#endif /* USE_LIBUV */
}

#ifndef USE_LIBUV
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
#endif /* USE_LIBUV */

/* Launched in HTTP server thread. */
static void
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

/* Launched in HTTP server thread. */
static void
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

/* Launched in TX thread. */
static void
tell_thread_to_terminate_internal(thread_ctx_t *thread_ctx)
{
	if (thread_ctx->shutdown_req_sent)
		return;
	thread_ctx->shutdown_req_sent = true;
	httpng_sem_wait(&thread_ctx->can_be_terminated);
#ifdef USE_LIBUV
	uv_async_send(&thread_ctx->terminate_notifier);
#else /* USE_LIBUV */
	while (write(thread_ctx->terminate_notifier.write_fd, "", 1) < 0
	    && errno == EINTR)
		;
#endif /* USE_LIBUV */
}

/* Launched in TX thread. */
static inline void
tell_thread_to_terminate_immediately(thread_ctx_t *thread_ctx)
{
#ifdef SUPPORT_GRACEFUL_THR_TERMINATION
	thread_ctx->use_graceful_shutdown = false;
#endif /* SUPPORT_GRACEFUL_THR_TERMINATION */
	__sync_synchronize();
	/* FIXME: Should we explicitly request connections to be closed?
	 * Thread may already be terminating gracefully. */
	tell_thread_to_terminate_internal(thread_ctx);
}

#ifdef SUPPORT_RECONFIG
/* Launched in TX thread. */
static inline void
tell_thread_to_terminate_gracefully(thread_ctx_t *thread_ctx)
{
#ifdef SUPPORT_GRACEFUL_THR_TERMINATION
	assert(thread_ctx->use_graceful_shutdown);
#endif /* SUPPORT_GRACEFUL_THR_TERMINATION */
	tell_thread_to_terminate_internal(thread_ctx);
}
#endif /* SUPPORT_RECONFIG */

/* Launched in TX thread. */
static void
configure_shutdown_callback(lua_State *L, bool setup)
{
	if (lua_pcall(L, 2, 0, 0) == LUA_OK) {
		conf.is_on_shutdown_setup = setup;
		if (!setup) {
			luaL_unref(L, LUA_REGISTRYINDEX, conf.on_shutdown_ref);
			conf.on_shutdown_ref = LUA_REFNIL;
		}
	} else
		fprintf(stderr, "Warning: box.ctl.on_shutdown() failed: %s\n",
			lua_tostring(L, -1));
}

/* Launched in TX thread. */
static void
setup_on_shutdown(lua_State *L, bool setup, bool called_from_callback)
{
	if (conf.on_shutdown_ref == LUA_REFNIL && !called_from_callback) {
		assert(setup);
		lua_pushcfunction(L, on_shutdown_callback);
		conf.on_shutdown_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	}

	lua_getglobal(L, "box");
	if (lua_type(L, -1) == LUA_TTABLE) {
		lua_getfield(L, -1, "ctl");
		if (lua_type(L, -1) == LUA_TTABLE) {
			lua_getfield(L, -1, "on_shutdown");
			if (lua_type(L, -1) == LUA_TFUNCTION) {
				if (setup) {
					lua_rawgeti(L, LUA_REGISTRYINDEX,
						conf.on_shutdown_ref);
					lua_pushnil(L);
					configure_shutdown_callback(L, setup);
				} else if (!called_from_callback) {
					lua_pushnil(L);
					lua_rawgeti(L, LUA_REGISTRYINDEX,
						conf.on_shutdown_ref);
					configure_shutdown_callback(L, setup);
				}
			} else
				fprintf(stderr,
	"Warning: global 'box.ctl.on_shutdown' is not a function\n");
		} else
			fprintf(stderr,
			"Warning: global 'box.ctl' is not a table\n");
	} else
		fprintf(stderr, "Warning: global 'box' is not a table\n");
}

/* Launched in TX thread. */
static void
wait_for_exiting_tx_fiber(thread_ctx_t *thread_ctx)
{
	struct fiber *const next_fiber_to_wake =
		thread_ctx->fiber_to_wake_on_shutdown;
	thread_ctx->fiber_to_wake_on_shutdown = fiber_self();
	fiber_yield();
	assert(thread_ctx->tx_fiber_finished);
	if (next_fiber_to_wake != NULL)
		fiber_wakeup(next_fiber_to_wake);
}

/* Launched in TX thread. */
static void
reap_finished_thread(thread_ctx_t *thread_ctx)
{
	pthread_join(thread_ctx->tid, NULL);
	destroy_h2o_context_and_loop(thread_ctx);

	struct fiber * *const tx_fiber = &conf.tx_fiber_ptrs[thread_ctx->idx];
	if (*tx_fiber != NULL) {
		if (thread_ctx->tx_fiber_finished) {
			fiber_join(*tx_fiber);
			*tx_fiber = NULL;
		} else {
			wait_for_exiting_tx_fiber(thread_ctx);
			if (*tx_fiber != NULL) {
				fiber_join(*tx_fiber);
				*tx_fiber = NULL;
			}
		}
	}

	free(thread_ctx->listener_ctxs);
	xtm_queue_delete(thread_ctx->queue_to_tx,
		XTM_QUEUE_MUST_CLOSE_PRODUCER_READFD |
		XTM_QUEUE_MUST_CLOSE_CONSUMER_READFD);
	my_xtm_delete_queue_from_tx(thread_ctx);
}

/* Launched in TX thread.
 * Returns error message in case of error. */
static const char *
reap_gracefully_terminating_threads(void)
{
	const char *result;
	assert(!(conf.reaping_flags & REAPING_GRACEFUL));
	conf.reaping_flags |= REAPING_GRACEFUL;
#ifdef SUPPORT_RECONFIG
	unsigned thr_idx;
	for (thr_idx = conf.num_threads - 1;
	    thr_idx >= conf.num_desired_threads; --thr_idx) {
		thread_ctx_t *const thread_ctx = &conf.thread_ctxs[thr_idx];
		if (!thread_ctx->thread_finished) {
			conf.num_threads = thr_idx + 1;
			result = msg_cant_reap;
			goto Exit;
		}
		reap_finished_thread(thread_ctx);
	}
	conf.num_threads = conf.num_desired_threads;
#endif /* SUPPORT_RECONFIG */
	result = NULL;
#ifdef SUPPORT_RECONFIG
Exit:
#endif /* SUPPORT_RECONFIG */
	conf.reaping_flags &= ~REAPING_GRACEFUL;
#ifdef SUPPORT_GRACEFUL_THR_TERMINATION
	if (conf.fiber_to_wake_on_reaping_done != NULL) {
		struct fiber *const fiber = conf.fiber_to_wake_on_reaping_done;
		conf.fiber_to_wake_on_reaping_done = NULL;
		fiber_wakeup(fiber);
	}
#endif /* SUPPORT_GRACEFUL_THR_TERMINATION */
	return result;
}

#ifdef SUPPORT_RECONFIG
/* Launched in HTTP server thread.
 * N. b.: It may never be launched if thread terminates. */
static void
become_ungraceful(thread_ctx_t *thread_ctx)
{
	close_existing_connections(thread_ctx);
}
#endif /* SUPPORT_RECONFIG */

/* Launched in TX thread. */
static void
reap_terminating_threads_ungracefully(void)
{
	assert(!(conf.reaping_flags & REAPING_UNGRACEFUL));
	if (!(conf.reaping_flags & REAPING_GRACEFUL) &&
	    reap_gracefully_terminating_threads() == NULL)
		return;

	conf.reaping_flags |= REAPING_UNGRACEFUL;
#ifdef SUPPORT_RECONFIG
	unsigned thr_idx;
	for (thr_idx = conf.num_threads - 1;
	    thr_idx >= conf.num_desired_threads; --thr_idx) {
		thread_ctx_t *const thread_ctx = &conf.thread_ctxs[thr_idx];
		if (thread_ctx->thread_finished)
			continue;
		call_in_http_thr_with_thread_ctx(become_ungraceful, thread_ctx);
	}

	for (thr_idx = conf.num_threads - 1;
	    thr_idx >= conf.num_desired_threads; --thr_idx) {
		thread_ctx_t *const thread_ctx = &conf.thread_ctxs[thr_idx];
		while (thread_ctx->active_lua_fibers ||
		    !thread_ctx->thread_finished)
			fiber_sleep(0.001);
	}
#endif /* SUPPORT_RECONFIG */

	if (conf.reaping_flags & REAPING_GRACEFUL) {
#ifdef SUPPORT_GRACEFUL_THR_TERMINATION
		assert(conf.fiber_to_wake_on_reaping_done == NULL);
		conf.fiber_to_wake_on_reaping_done = fiber_self();
		fiber_yield();
#endif /* SUPPORT_GRACEFUL_THR_TERMINATION */
		assert(!(conf.reaping_flags & REAPING_GRACEFUL));
	}
	const char *const err = reap_gracefully_terminating_threads();
	(void)err;
	assert(err == NULL);
	conf.reaping_flags &= ~REAPING_UNGRACEFUL;
}

#ifdef SUPPORT_GRACEFUL_THR_TERMINATION
/* Launched in TX thread. */
static void
terminate_reaper_fiber(void)
{
	if (conf.reaper_fiber == NULL)
		return;
	if (conf.reaper_exited) {
		fiber_join(conf.reaper_fiber);
		conf.reaper_fiber = NULL;
		return;
	}
	assert(conf.fiber_to_wake_by_reaper_fiber == NULL);
	conf.fiber_to_wake_by_reaper_fiber = fiber_self();
	conf.reaper_should_exit = true;
	fiber_wakeup(conf.reaper_fiber);
	if (!conf.reaper_exited) {
		fiber_yield();
		assert(conf.reaper_exited);
	}
	fiber_join(conf.reaper_fiber);
	conf.reaper_fiber = NULL;
}
#endif /* SUPPORT_GRACEFUL_THR_TERMINATION */

/* Launched in TX thread. */
static void
dispose_h2o_configs(void)
{
	unsigned thr_idx;
	for (thr_idx = 0; thr_idx < MAX_threads; ++thr_idx)
		h2o_config_dispose(&conf.thread_ctxs[thr_idx].globalconf);
}

/* Launched in TX thread. */
static int
on_shutdown_internal(lua_State *L, bool called_from_callback)
{
	while (conf.cfg_in_progress)
		fiber_sleep(0.001);
	if (!conf.configured)
		return luaL_error(L, "Server is not launched");
	if (conf.inject_shutdown_error && !called_from_callback)
		return luaL_error(L,
			"Debugging: simulating broken shutdown support");
	if (conf.is_shutdown_in_progress) {
		if (!called_from_callback)
			return luaL_error(L,
				"on_shutdown() is already in progress");
		fprintf(stderr,
			"Warning: on_shutdown() is already in progress\n");
		return 0;
	}
	conf.is_shutdown_in_progress = true;
#ifdef SUPPORT_GRACEFUL_THR_TERMINATION
	terminate_reaper_fiber();
#endif /* SUPPORT_GRACEFUL_THR_TERMINATION */
	reap_terminating_threads_ungracefully();
	if (conf.is_on_shutdown_setup)
		setup_on_shutdown(L, false, called_from_callback);
	unsigned thr_idx;
	for (thr_idx = 0; thr_idx < conf.num_threads; ++thr_idx) {
		thread_ctx_t *const thread_ctx =
			&conf.thread_ctxs[thr_idx];
		tell_thread_to_terminate_immediately(thread_ctx);
	}

	for (thr_idx = 0; thr_idx < conf.num_threads; ++thr_idx) {
		thread_ctx_t *const thread_ctx =
			&conf.thread_ctxs[thr_idx];
		/* We must yield CPU to other fibers to finish. */
		while (thread_ctx->active_lua_fibers ||
		    !thread_ctx->thread_finished)
			fiber_sleep(0.001);
		reap_finished_thread(thread_ctx);
		assert(thread_ctx->shuttle_counter == 0);
#ifdef USE_SHUTTLES_MUTEX
		pthread_mutex_destroy(&thread_ctx->shuttles_mutex);
#endif /* USE_SHUTTLES_MUTEX */
	}
	deinit_listener_cfgs();
#ifdef USE_LIBUV
	for (idx = 0; idx < conf.num_listeners; ++idx) {
		close(conf.listener_cfgs[idx].fd);
		for (thr_idx = 1; thr_idx < conf.num_threads; ++thr_idx)
			close(conf.thread_ctxs[thr_idx].listener_ctxs[idx].fd);
	}
#endif /* USE_LIBUV */
	free(conf.listener_cfgs);
	conf_sni_map_cleanup();
	dispose_h2o_configs();
	free(conf.thread_ctxs);
	luaL_unref(L, LUA_REGISTRYINDEX, conf.lua_handler_ref);
	conf.configured = false;
#ifdef SUPPORT_C_ROUTER
	conf.fill_router_data = NULL;
#endif /* SUPPORT_C_ROUTER */
	complain_loudly_about_leaked_fds();
	conf.is_shutdown_in_progress = false;
	return 0;
}

/* Launched in TX thread. */
static int
on_shutdown_callback(lua_State *L)
{
	return on_shutdown_internal(L, true);
}

/* Launched in TX thread. */
static int
on_shutdown_for_user(lua_State *L)
{
	return on_shutdown_internal(L, false);
}

#ifdef SUPPORT_RECONFIG
/* Launched in TX thread. */
static void
replace_lua_handlers(lua_State *L)
{
	luaL_unref(L, LUA_REGISTRYINDEX, conf.lua_handler_ref);
	conf.lua_handler_ref = conf.new_lua_handler_ref;
}
#endif /* SUPPORT_RECONFIG */

/* Launched in TX thread. */
static void
prepare_thread_ctx(unsigned thread_idx)
{
	thread_ctx_t *const thread_ctx = &conf.thread_ctxs[thread_idx];
	thread_ctx->idx = thread_idx;
	thread_ctx->listeners_created = 0;
	thread_ctx->num_connections = 0;
	thread_ctx->active_lua_fibers = 0;
	thread_ctx->fiber_to_wake_on_shutdown = NULL;
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
		httpng_sem_init(&thread_ctx->can_be_terminated, 0);
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
	unsigned idx;
	for (idx = start_idx; idx < start_idx_plus_len; ++idx) {
		struct fiber *const fiber = conf.tx_fiber_ptrs[idx];
		if (fiber == NULL)
			continue;

		thread_ctx_t *const thread_ctx = &conf.thread_ctxs[idx];

		/* Do not use fiber_cancel() because state variables
		 * may not be set.
		 * Using xtm is safe because threads have finished. */
		if (!thread_ctx->tx_fiber_should_exit)
			call_from_tx(thread_ctx->queue_to_tx,
				tell_tx_fiber_to_exit, thread_ctx, thread_ctx);
	}
	for (idx = start_idx; idx < start_idx_plus_len; ++idx) {
		thread_ctx_t *const thread_ctx = &conf.thread_ctxs[idx];
		if (thread_ctx->tx_fiber_finished)
			continue;

		wait_for_exiting_tx_fiber(thread_ctx);
	}
	for (idx = start_idx; idx < start_idx_plus_len; ++idx) {
		struct fiber * *const fiber = &conf.tx_fiber_ptrs[idx];
		if (*fiber == NULL)
			continue;
		fiber_join(*fiber);
		*fiber = NULL;
	}
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
	unsigned idx;
	for (idx = start_idx; idx < start_idx_plus_len; ++idx)
		tell_thread_to_terminate_immediately(&conf.thread_ctxs[idx]);

	for (idx = start_idx; idx < start_idx_plus_len; ++idx)
		pthread_join(conf.thread_ctxs[idx].tid, NULL);
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

#ifdef SUPPORT_RECONFIG
/* Launched in TX thread. */
static const char *
hot_reload_add_threads(unsigned threads)
{
	const char *lerr = NULL;
	unsigned xtm_to_tx_idx = conf.num_threads;
	if ((lerr = create_xtm_queues_to_tx(&xtm_to_tx_idx, threads)) != NULL)
		goto add_thr_xtm_to_tx_fail;

	unsigned fiber_idx = conf.num_threads;
	if ((lerr = start_tx_fibers(&fiber_idx, threads)) != NULL)
		goto add_thr_fibers_fail;

	unsigned thr_init_idx;
	for (thr_init_idx = conf.num_threads; thr_init_idx < threads;
	    ++thr_init_idx)
		if (!init_worker_thread(thr_init_idx)) {
			lerr = "Failed to init worker threads";
			goto add_thr_threads_init_fail;
		}

	__sync_synchronize();

	unsigned thr_launch_idx = conf.num_threads;
	if ((lerr = start_worker_threads(&thr_launch_idx, threads)) != NULL)
		goto add_thr_threads_launch_fail;

	return lerr;

add_thr_threads_launch_fail:
	/* FIXME: We should not ungracefully terminate
	 * successfully added threads, doing this would fail some requests. */
	terminate_and_join_threads(conf.num_threads, thr_launch_idx);

add_thr_threads_init_fail:
	deinit_worker_threads(conf.num_threads, thr_init_idx);
add_thr_fibers_fail:
	terminate_tx_fibers(conf.num_threads, fiber_idx);
add_thr_xtm_to_tx_fail:
	xtm_delete_queues_to_tx(conf.num_threads, xtm_to_tx_idx);

	return lerr;
}
#endif /* SUPPORT_RECONFIG */

#ifdef SUPPORT_GRACEFUL_THR_TERMINATION
/* Launched in TX thread. */
static int
reaper_fiber_func(va_list ap)
{
	(void)ap;

	while (1) {
		const double now = fiber_clock();
		if (conf.reaping_flags == 0 && !conf.cfg_in_progress &&
		    !conf.is_shutdown_in_progress) {
			if (reap_gracefully_terminating_threads() == NULL)
				conf.is_thr_term_timeout_active = false;
			else if (conf.is_thr_term_timeout_active &&
			    now - conf.thr_timeout_start >=
				    conf.thread_termination_timeout &&
			    conf.reaping_flags == 0 &&
			    !conf.cfg_in_progress &&
			    !conf.is_shutdown_in_progress) {
				conf.is_thr_term_timeout_active = false;
				reap_terminating_threads_ungracefully();
			}
		}

		if (conf.fiber_to_wake_by_reaper_fiber != NULL) {
			struct fiber *const fiber =
				conf.fiber_to_wake_by_reaper_fiber;
			conf.fiber_to_wake_by_reaper_fiber = NULL;
			fiber_wakeup(fiber);
		}
		if (conf.reaper_should_exit) {
			conf.reaper_exited = true;
			return 0;
		}

		if (conf.is_thr_term_timeout_active) {
			const double remain = conf.thr_timeout_start +
				conf.thread_termination_timeout - now;
			if (remain >= 0)
				fiber_sleep(remain);
			else {
				/* Should not really happen, but... */
				conf.thr_timeout_start = now;
				fiber_sleep(conf.thread_termination_timeout);
			}
		} else
			fiber_yield();
	}
}
#endif /* SUPPORT_GRACEFUL_THR_TERMINATION */

#ifdef SUPPORT_RECONFIG
/* Launched in TX thread. */
static void
deactivate_reaper_fiber(void)
{
#ifdef SUPPORT_GRACEFUL_THR_TERMINATION
	conf.is_thr_term_timeout_active = false;
	/* There is no point in awaking it. */
#endif /* SUPPORT_GRACEFUL_THR_TERMINATION */
}
#endif /* SUPPORT_RECONFIG */

#ifdef SUPPORT_RECONFIG
/* Launched in TX thread. */
static void
hot_reload_remove_threads(unsigned threads)
{
	if (threads >= conf.num_threads)
		return;

	conf.num_desired_threads = threads;

	unsigned thr_idx;
	for (thr_idx = threads; thr_idx < conf.num_threads; ++thr_idx) {
		thread_ctx_t *const thread_ctx = &conf.thread_ctxs[thr_idx];
		tell_thread_to_terminate_gracefully(thread_ctx);
	}

#ifdef SUPPORT_GRACEFUL_THR_TERMINATION
	if (conf.thread_termination_timeout <= 0)
#endif /* SUPPORT_GRACEFUL_THR_TERMINATION */
	{
		deactivate_reaper_fiber();
		return;
	}

#ifdef SUPPORT_GRACEFUL_THR_TERMINATION
	const double now = fiber_clock();
	conf.thr_timeout_start = now;
	conf.is_thr_term_timeout_active = true;
	assert(conf.reaper_fiber != NULL);
	fiber_wakeup(conf.reaper_fiber);
#endif /* SUPPORT_GRACEFUL_THR_TERMINATION */
}
#endif /* SUPPORT_RECONFIG */

#ifdef SUPPORT_GRACEFUL_THR_TERMINATION
/* Launched in TX thread. */
static void
configure_and_start_reaper_fiber(void)
{
	fiber_set_joinable(conf.reaper_fiber, true);
	conf.reaper_exited = false;
	conf.reaper_should_exit = false;
	conf.fiber_to_wake_by_reaper_fiber = NULL;
	conf.fiber_to_wake_on_reaping_done = NULL;
	conf.is_thr_term_timeout_active = false;
	conf.reaping_flags = 0;
	fiber_start(conf.reaper_fiber);
}
#endif /* SUPPORT_GRACEFUL_THR_TERMINATION */

/* Launched in TX thread. */
static inline const char *
parse_min_proto_version(const char *min_proto_version_str,
	size_t min_proto_version_len, bool is_reconfig)
{

#define FILL_PROTO_STR(name, value) \
	{ (name), sizeof(name) - 1, (value) }

	static struct {
		char str[8];
		size_t len;
		long num;
	} protos[] = {
		FILL_PROTO_STR(SSL3_STR, SSL3_VERSION),
		FILL_PROTO_STR(TLS1_STR, TLS1_VERSION),
		FILL_PROTO_STR(TLS1_0_STR, TLS1_VERSION),
		FILL_PROTO_STR(TLS1_1_STR, TLS1_1_VERSION),
		FILL_PROTO_STR(TLS1_2_STR, TLS1_2_VERSION),
#ifdef TLS1_3_VERSION
		FILL_PROTO_STR(TLS1_3_STR, TLS1_3_VERSION),
#endif /* TLS1_3_VERSION */
	};
#undef FILL_PROTO_STR
	unsigned idx;
	for (idx = 0; idx < lengthof(protos); ++idx) {
		if (protos[idx].len == min_proto_version_len &&
		    !memcmp(&protos[idx].str, min_proto_version_str,
		    min_proto_version_len)) {
			const long min_tls_proto_version = protos[idx].num;
#ifdef SUPPORT_RECONFIG
			if (is_reconfig) {
			       if (conf.min_tls_proto_version !=
				    min_tls_proto_version)
					return min_proto_version_reconf;
			} else
#else /* SUPPORT_RECONFIG */
			assert(!is_reconfig);
#endif /* SUPPORT_RECONFIG */
				conf.min_tls_proto_version =
					min_tls_proto_version;
			return NULL;
		}
	}

	/* This is security, do not silently fall back to defaults. */
	return "unknown min_proto_version specified";
}

#ifdef SUPPORT_GRACEFUL_THR_TERMINATION
/* Launched in TX thread. */
static const char *
get_thread_termination_timeout(lua_State *L,
	int idx, double *thread_termination_timeout)
{
	lua_getfield(L, idx, "thread_termination_timeout");
	if (is_nil_or_null(L, -1)) {
		*thread_termination_timeout =
			DEFAULT_thread_termination_timeout;
		return NULL;
	}
	int is_number;
	*thread_termination_timeout = my_lua_tonumberx(L, -1, &is_number);
	return is_number ? NULL :
		"parameter thread_termination_timeout is not a number";
}
#endif /* SUPPORT_GRACEFUL_THR_TERMINATION */

/* Launched in TX thread. */
static const char *
get_min_proto_version(lua_State *L, int idx, bool is_reconfig)
{
	lua_getfield(L, idx, "min_proto_version");
	if (is_nil_or_null(L, -1)) {
#ifdef SUPPORT_RECONFIG
		if (is_reconfig) {
			if (is_box_null(L, -1) && conf.min_tls_proto_version !=
			    DEFAULT_MIN_TLS_PROTO_VERSION_NUM)
				return min_proto_version_reconf;
		} else
#else /* SUPPORT_RECONFIG */
		assert(!is_reconfig);
#endif /* SUPPORT_RECONFIG */
		{
			conf.min_tls_proto_version =
				DEFAULT_MIN_TLS_PROTO_VERSION_NUM;
			fprintf(stderr, "Using default min_proto_version="
				DEFAULT_MIN_TLS_PROTO_VERSION_STR "\n");
		}
		return NULL;
	}

	static const char err_msg[] = "min_proto_version is not a string";
	if (!lua_isstring_strict(L, -1))
		return err_msg;
	size_t min_proto_version_len;
	const char *const min_proto_version_str =
		lua_tolstring(L, -1, &min_proto_version_len);
	if (min_proto_version_str == NULL)
		return err_msg;

	return parse_min_proto_version(min_proto_version_str,
		min_proto_version_len, is_reconfig);
}

/* Launched in TX thread. */
static const char *
get_openssl_security_level(lua_State *L, int idx, bool is_reconfig)
{
	lua_getfield(L, idx, "openssl_security_level");
	if (is_nil_or_null(L, -1)) {
#ifdef SUPPORT_RECONFIG
		if (is_reconfig) {
			if (is_box_null(L, -1) && conf.openssl_security_level !=
			    DEFAULT_OPENSSL_SECURITY_LEVEL)
				return openssl_security_level_reconf;
		} else
#else /* SUPPORT_RECONFIG */
		assert(!is_reconfig);
#endif /* SUPPORT_RECONFIG */
		{
			conf.openssl_security_level =
				DEFAULT_OPENSSL_SECURITY_LEVEL;
			fprintf(stderr,
				"Using default openssl_security_level=%d\n",
				DEFAULT_OPENSSL_SECURITY_LEVEL);
		}
		return NULL;
	}

	int is_integer;
	const uint64_t openssl_security_level =
		my_lua_tointegerx(L, -1, &is_integer);
	if (!is_integer)
		return "openssl_security_level is not a number";
	if (openssl_security_level > 5)
		return "openssl_security_level is invalid";
#ifdef SUPPORT_RECONFIG
	if (is_reconfig) {
		if (conf.openssl_security_level != openssl_security_level)
			return openssl_security_level_reconf;
	} else
#else /* SUPPORT_RECONFIG */
	assert(!is_reconfig);
#endif /* SUPPORT_RECONFIG */
		conf.openssl_security_level = openssl_security_level;
	return NULL;
}

#ifdef SUPPORT_RECONFIG
/* Launched in TX thread. */
static const char *
get_handler_reconfig(lua_State *L, int idx, bool *handler_replaced)
{
	lua_getfield(L, idx, "handler");
	if (lua_isnil(L, -1))
		return NULL;
	if (lua_type(L, -1) != LUA_TFUNCTION &&
	    lua_type(L, -1) != LUA_TTABLE)
		return "handler is not a function or table (router object)";

	if (is_router_used())
		return (lua_type(L, -1) == LUA_TFUNCTION) ?
			"Replacing router with handler is not supported yet" :
			"Replacing router is not supported yet";
	if (lua_type(L, -1) != LUA_TFUNCTION)
		return "Replacing handler with router is not supported yet";
	conf.new_lua_handler_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	*handler_replaced = true;
	return NULL;
}
#endif /* SUPPORT_RECONFIG */

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
		const char *lerr;
		if (register_router(L,
		    luaL_ref(L, LUA_REGISTRYINDEX), &lerr) != 0)
			return lerr;
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
#ifdef SUPPORT_GRACEFUL_THR_TERMINATION
	, double thread_termination_timeout
#endif /* SUPPORT_GRACEFUL_THR_TERMINATION */
#ifdef SUPPORT_SPLITTING_LARGE_BODY
	, bool use_body_split
#endif /* SUPPORT_SPLITTING_LARGE_BODY */
	)
{
#ifdef SUPPORT_SPLITTING_LARGE_BODY
	conf.use_body_split = use_body_split;
#endif /* SUPPORT_SPLITTING_LARGE_BODY */
	conf.max_conn_per_thread = max_conn_per_thread;
	conf.max_shuttles_per_thread = max_shuttles_per_thread;
#ifndef USE_LIBUV
	conf.num_accepts = max_conn_per_thread / 16;
	if (conf.num_accepts < 8)
		conf.num_accepts = 8;
#endif /* USE_LIBUV */
	apply_max_body_len(max_body_len);
#ifdef SUPPORT_GRACEFUL_THR_TERMINATION
	conf.thread_termination_timeout = thread_termination_timeout;
#endif /* SUPPORT_GRACEFUL_THR_TERMINATION */
}

/* Launched in TX thread. */
static const char *
get_c_handlers(lua_State *L, int idx, const path_desc_t **path_descs_ptr)
{
	lua_getfield(L, idx, "c_sites_func");
	if (lua_isnil(L, -1))
		return NULL;

	if (lua_type(L, -1) != LUA_TFUNCTION)
		return "c_sites_func must be a function";

	lua_getfield(L, idx, "c_sites_func_param");
	if (lua_pcall(L, 1, 1, 0) != LUA_OK)
		return "c_sites_func() failed";

	if (!lua_islightuserdata(L, -1))
		return "c_sites_func() returned wrong data type";
	*path_descs_ptr = (path_desc_t *)lua_touserdata(L, -1);
	return NULL;
}

/* Launched in TX thread. */
static const char *
register_c_handlers(const path_desc_t *path_descs,
	unsigned *count_ptr)
{
	if (path_descs == NULL) {
		*count_ptr = 0;
		return NULL;
	}

	const path_desc_t *path_desc = path_descs;
	if (path_desc->path == NULL)
		/* Need at least one. */
		return "Empty C sites list";

	do {
		unsigned thread_idx;
		for (thread_idx = 0; thread_idx < MAX_threads;
		    ++thread_idx)
			register_handler(conf.thread_ctxs[thread_idx]
				.hostconf, path_desc->path,
				path_desc->handler);
	} while ((++path_desc)->path != NULL);
	*count_ptr = path_desc - path_descs;
	return NULL;
}

/* Launched in TX thread. */
static const char *
register_hosts(void)
{
	unsigned idx;
	for (idx = 0; idx < MAX_threads; ++idx)
		h2o_config_init(&conf.thread_ctxs[idx].globalconf);

	for (idx = 0; idx < MAX_threads; ++idx) {
		thread_ctx_t *const thread_ctx =
			&conf.thread_ctxs[idx];
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
	unsigned *lua_site_count_ptr, unsigned c_handlers_count)
{
	const char *lerr;
	unsigned lua_site_count = 0;

	assert(*lua_site_count_ptr == 0);
	if ((lerr = get_handler_not_reconfig(L, idx,
	    &lua_site_count)) != NULL)
		return lerr;

	*lua_site_count_ptr = lua_site_count;
	if (c_handlers_count + lua_site_count == 0)
		return "No handlers specified";

	if (lua_site_count != 0 && conf.shuttle_size < sizeof(shuttle_t) +
	    sizeof(lua_handler_state_t))
		return "shuttle_size is too small for Lua handlers";

	if ((lerr = get_min_proto_version(L, idx, false)) != NULL)
		return lerr;

	if ((lerr = get_openssl_security_level(L, idx, false)) != NULL)
		return lerr;

	if (load_and_handle_listen_from_lua(L, idx, &lerr) != 0)
		return lerr;

	return NULL;
}

#ifdef SUPPORT_RECONFIG
/* Launched in TX thread. */
static const char *
reconfig_handler_security_listen_threads(lua_State *L, int idx,
	unsigned threads, bool *handler_replaced)
{
	const char *lerr;
	if ((lerr = get_handler_reconfig(L, idx, handler_replaced)) != NULL)
		return lerr;

	if ((lerr = get_min_proto_version(L, idx, true)) != NULL)
		return lerr;

	if ((lerr = get_openssl_security_level(L, idx, true)) != NULL)
		return lerr;

	lua_getfield(L, idx, "listen");
	if (!lua_isnil(L, -1))
		return "listen can't be changed on reconfiguration";

	if (threads > conf.num_threads) {
		lerr = hot_reload_add_threads(threads);
		if (lerr != NULL)
			return lerr;
		conf.num_desired_threads = conf.num_threads = threads;
	}
	return NULL;
}

/* Launched in TX thread. */
static void
unref_on_reconfig_failure(lua_State *L, bool handler_replaced)
{
	if (!handler_replaced)
		return;
	luaL_unref(L, LUA_REGISTRYINDEX, conf.new_lua_handler_ref);
}
#endif /* SUPPORT_RECONFIG */

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

#ifdef SUPPORT_GRACEFUL_THR_TERMINATION
/* Launched in TX thread. */
static const char *
launch_reaper_fiber(void)
{
	if ((conf.reaper_fiber =
	    fiber_new("reaper fiber", reaper_fiber_func)) == NULL)
		return "Failed to create reaper fiber";
	configure_and_start_reaper_fiber();
	return NULL;
}
#endif /* SUPPORT_GRACEFUL_THR_TERMINATION */

/* Launched in TX thread. */
static const char *
init_userdata(const path_desc_t *path_descs)
{
	if (path_descs == NULL)
		return NULL;
	const path_desc_t *path_desc = path_descs;
	do {
		if (path_desc->init_userdata_in_tx != NULL &&
		    path_desc->init_userdata_in_tx(
			    path_desc->init_userdata_in_tx_param))
			return "Failed to init userdata";
	} while ((++path_desc)->path != NULL);
	return NULL;
}

#ifdef SUPPORT_SPLITTING_LARGE_BODY
/* Launched in TX thread. */
static inline bool
get_use_body_split(lua_State *L, int idx, unsigned shuttle_size,
	uint64_t *max_body_len)
{
	lua_getfield(L, idx, "use_body_split");
	const bool use_body_split =
		is_nil_or_null(L, -1) ? false : lua_toboolean(L, -1);
	if (!use_body_split && *max_body_len >
	    (shuttle_size - sizeof(shuttle_t)))
		*max_body_len = shuttle_size - sizeof(shuttle_t);
	return use_body_split;
}
#endif /* SUPPORT_SPLITTING_LARGE_BODY */

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

#ifdef SUPPORT_SPLITTING_LARGE_BODY

#define PROCESS_MAX_BODY_LEN() PROCESS_OPTIONAL_PARAM(max_body_len)

#else /* SUPPORT_SPLITTING_LARGE_BODY */

#define PROCESS_MAX_BODY_LEN() \
	const uint64_t max_body_len = shuttle_size - sizeof(shuttle_t)

#endif /* SUPPORT_SPLITTING_LARGE_BODY */

/* N. b.: This macro uses goto. */
#define PROCESS_OPTIONAL_PARAMS() \
	; \
	int is_integer; \
	PROCESS_OPTIONAL_PARAM(threads); \
	PROCESS_OPTIONAL_PARAM(max_conn_per_thread); \
	PROCESS_OPTIONAL_PARAM(max_shuttles_per_thread); \
	PROCESS_OPTIONAL_PARAM(shuttle_size); \
	PROCESS_MAX_BODY_LEN(); \
	/* FIXME: Maybe we need to configure tfo_queues? */

#ifdef SUPPORT_RECONFIG
/* Launched in TX thread. */
static int
reconfigure(lua_State *L)
{
	/* Lua parameters: cfg table. */
	enum {
		LUA_STACK_IDX_TABLE = 1,
	};
	assert(conf.configured);
	const char *lerr = NULL; /* Error message for caller. */
	if (conf.hot_reload_in_progress) {
		lerr = "Reconfiguration is already in progress";
		goto error_something;
	}
	while (conf.reaping_flags != 0)
		/* This can be done more efficiently
		 * but would require more logic - is it worth it? */
		fiber_sleep(0.001);
	if ((lerr = reap_gracefully_terminating_threads()) != NULL)
		goto error_something;
	conf.hot_reload_in_progress = true;

	lua_getfield(L, LUA_STACK_IDX_TABLE, "c_sites_func");
	if (!lua_isnil(L, -1)) {
		lerr = "Reconfiguration can't be used with C handlers";
		goto error_hot_reload_c;
	}

	/* N. b.: This macro uses goto. */
	PROCESS_OPTIONAL_PARAMS();

	if (conf.shuttle_size != shuttle_size) {
		lerr = "Reconfiguration can't change shuttle_size";
		goto error_hot_reload_shuttle_size;
	}

#ifdef SUPPORT_GRACEFUL_THR_TERMINATION
	double thread_termination_timeout;
	if ((lerr = get_thread_termination_timeout(L, LUA_STACK_IDX_TABLE,
	    &thread_termination_timeout)) != NULL)
		goto error_parameter_not_a_number;
#endif /* SUPPORT_GRACEFUL_THR_TERMINATION */

#ifdef SUPPORT_SPLITTING_LARGE_BODY
	const bool use_body_split = get_use_body_split(L, LUA_STACK_IDX_TABLE,
		shuttle_size, &max_body_len);
#endif /* SUPPORT_SPLITTING_LARGE_BODY */

	bool handler_replaced = false;
	if ((lerr = reconfig_handler_security_listen_threads(L,
	    LUA_STACK_IDX_TABLE, threads, &handler_replaced)) != NULL)
		goto invalid_handler;

	apply_new_config(max_body_len, max_conn_per_thread,
		max_shuttles_per_thread
#ifdef SUPPORT_GRACEFUL_THR_TERMINATION
		, thread_termination_timeout
#endif /* SUPPORT_GRACEFUL_THR_TERMINATION */
#ifdef SUPPORT_SPLITTING_LARGE_BODY
		, use_body_split
#endif /* SUPPORT_SPLITTING_LARGE_BODY */
		);

	if (handler_replaced)
	       replace_lua_handlers(L);
	hot_reload_remove_threads(threads);

	conf.hot_reload_in_progress = false;
	conf.cfg_in_progress = false;
	return 0;

invalid_handler:
	unref_on_reconfig_failure(L, handler_replaced);
error_hot_reload_shuttle_size:
error_parameter_not_a_number:
error_hot_reload_c:
	conf.hot_reload_in_progress = false;
error_something:
	conf.cfg_in_progress = false;
	assert(lerr != NULL);
	return luaL_error(L, lerr);
}
#endif /* SUPPORT_RECONFIG */

/* Launched in TX thread. */
static inline void
save_params_to_conf(unsigned shuttle_size, unsigned threads)
{
	/* FIXME: Add sanity checks, especially shuttle_size -
	 * it must >sizeof(shuttle_t) (accounting for Lua payload)
	 * and aligned. */
#ifdef SUPPORT_RECONFIG
	conf.num_desired_threads =
#endif /* SUPPORT_RECONFIG */
	conf.num_threads = threads;
	conf.shuttle_size = shuttle_size;

	/* FIXME: Can differ from shuttle_size. */
	conf.recv_data_size = shuttle_size;

	conf.max_headers_lua = (conf.shuttle_size - sizeof(shuttle_t) -
		offsetof(lua_handler_state_t, un.resp.first.headers)) /
		sizeof(http_header_entry_t);
	conf.max_path_len_lua = conf.shuttle_size - sizeof(shuttle_t) -
		offsetof(lua_handler_state_t, un.req.buffer);
	conf.max_recv_bytes_lua_websocket = conf.recv_data_size -
		(uintptr_t)get_websocket_recv_location(NULL);
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
	if (conf.is_shutdown_in_progress) {
		lerr = "shutdown is in progress";
		goto error_something;
	}
	if (conf.configured)
#ifdef SUPPORT_RECONFIG
		return reconfigure(L);
#else /* SUPPORT_RECONFIG */
	{
		lerr = "Server is already launched";
		goto error_something;
	}
#endif /* SUPPORT_RECONFIG */

	const path_desc_t *path_descs = NULL;
	if ((lerr = get_c_handlers(L, LUA_STACK_IDX_TABLE, &path_descs)) !=
	    NULL)
		goto error_c_sites;

	/* N. b.: This macro uses goto. */
	PROCESS_OPTIONAL_PARAMS();

#ifdef SUPPORT_GRACEFUL_THR_TERMINATION
	double thread_termination_timeout;
	if ((lerr = get_thread_termination_timeout(L, LUA_STACK_IDX_TABLE,
	    &thread_termination_timeout)) != NULL)
		goto error_parameter_not_a_number;
#endif /* SUPPORT_GRACEFUL_THR_TERMINATION */

#ifdef SUPPORT_SPLITTING_LARGE_BODY
	const bool use_body_split = get_use_body_split(L, LUA_STACK_IDX_TABLE,
		shuttle_size, &max_body_len);
#endif /* SUPPORT_SPLITTING_LARGE_BODY */

	save_params_to_conf(shuttle_size, threads);

	if ((conf.thread_ctxs = (thread_ctx_t *)malloc(MAX_threads *
	    sizeof(thread_ctx_t))) == NULL) {
		lerr = "Failed to allocate memory for thread contexts";
		goto thread_ctxs_alloc_failed;
	}

	prepare_thread_ctxs();

	if ((lerr = register_hosts()) != NULL)
		goto register_host_failed;

	unsigned c_handlers_count = 0;
	if ((lerr = register_c_handlers(path_descs, &c_handlers_count)) !=
	    NULL)
		goto c_desc_empty;

	unsigned lua_site_count = 0;

	if ((lerr = configure_handler_security_listen(L, LUA_STACK_IDX_TABLE,
	    &lua_site_count, c_handlers_count)) != NULL)
		goto invalid_handler;

#if 0
	/* FIXME: Should make customizable. */
	/* Never returns NULL. */
	h2o_logger_t *logger = h2o_access_log_register(&config.default_host,
		"/dev/stdout", NULL);
#endif

	unsigned xtm_to_tx_idx = 0;
	if ((lerr = create_xtm_queues_to_tx(&xtm_to_tx_idx, conf.num_threads))
	    != NULL)
		goto xtm_to_tx_fail;

	unsigned fiber_idx = 0;
	if ((lerr = start_tx_fibers(&fiber_idx, conf.num_threads)) != NULL)
		goto fibers_fail;
#ifdef SUPPORT_GRACEFUL_THR_TERMINATION
	if ((lerr = launch_reaper_fiber()) != NULL)
		goto reaper_fiber_fail;
#endif /* SUPPORT_GRACEFUL_THR_TERMINATION */
	if ((lerr = init_userdata(path_descs)) != NULL)
		goto userdata_init_fail;
	unsigned thr_init_idx = 0;
	for (; thr_init_idx < conf.num_threads; ++thr_init_idx)
		if (!init_worker_thread(thr_init_idx)) {
			lerr = "Failed to init worker threads";
			goto threads_init_fail;
		}

	apply_new_config(max_body_len, max_conn_per_thread,
		max_shuttles_per_thread
#ifdef SUPPORT_GRACEFUL_THR_TERMINATION
		, thread_termination_timeout
#endif /* SUPPORT_GRACEFUL_THR_TERMINATION */
#ifdef SUPPORT_SPLITTING_LARGE_BODY
		, use_body_split
#endif /* SUPPORT_SPLITTING_LARGE_BODY */
		);

	__sync_synchronize();

	unsigned thr_launch_idx = 0;
	if ((lerr = start_worker_threads(&thr_launch_idx, conf.num_threads)) !=
	    NULL)
		goto threads_launch_fail;

	if (!conf.is_on_shutdown_setup)
		setup_on_shutdown(L, true, false);
	conf.configured = true;
	conf.cfg_in_progress = false;
	return 0;

threads_launch_fail:
	terminate_and_join_threads(0, thr_launch_idx);
threads_init_fail:
	deinit_worker_threads(0, thr_init_idx);
userdata_init_fail:
#ifdef SUPPORT_GRACEFUL_THR_TERMINATION
	terminate_reaper_fiber();
reaper_fiber_fail:
#endif /* SUPPORT_GRACEFUL_THR_TERMINATION */
fibers_fail:
	terminate_tx_fibers(0, fiber_idx);
xtm_to_tx_fail:
	xtm_delete_queues_to_tx(0, xtm_to_tx_idx);
	close_listener_cfgs_sockets();
	deinit_listener_cfgs();
	conf_sni_map_cleanup();
invalid_handler:
	unref_on_config_failure(L, lua_site_count);
c_desc_empty:
register_host_failed:
	/* N.b.: h2o currently can't "unregister" host(s). */
	dispose_h2o_configs();
	free(conf.thread_ctxs);
thread_ctxs_alloc_failed:
error_parameter_not_a_number:
error_c_sites:
error_something:
error_no_parameters:
	conf.cfg_in_progress = false;
	assert(lerr != NULL);
	return luaL_error(L, lerr);
}

#undef PROCESS_OPTIONAL_PARAMS
#undef PROCESS_OPTIONAL_PARAM

/* Can be launched in TX thread or HTTP server thread. */
unsigned
get_shuttle_size(void)
{
	assert(conf.shuttle_size >= MIN_shuttle_size);
	assert(conf.shuttle_size <= MAX_shuttle_size);
	return conf.shuttle_size;
}

#ifdef SUPPORT_RECONFIG
/* Launched in TX thread. */
static int
force_decrease_threads(lua_State *L)
{
	if (!conf.configured) {
		return luaL_error(L,
			"Not configured, nothing to terminate");
	}

	deactivate_reaper_fiber();
	while (conf.reaping_flags & REAPING_UNGRACEFUL)
		/* This can be done more efficiently
		 * but would require more logic. */
		fiber_sleep(0.001);

	if (conf.is_shutdown_in_progress)
		return luaL_error(L, "Shutdown is in progress");
	deactivate_reaper_fiber();
	reap_terminating_threads_ungracefully();
	return 0;
}
#endif /* SUPPORT_RECONFIG */

/* Launched in TX thread. */
static int
cfg_debug(lua_State *L)
{
	/* Lua parameters: table. */
	enum {
		LUA_STACK_DEBUG_IDX_TABLE = 1,
		LUA_STACK_REQUIRED_PARAMS_COUNT = LUA_STACK_DEBUG_IDX_TABLE,
	};
	const char *lerr = NULL;
	if (lua_gettop(L) < LUA_STACK_REQUIRED_PARAMS_COUNT) {
		lerr = "No parameters specified";
		goto error_no_parameters;
	}
	lua_getfield(L, LUA_STACK_DEBUG_IDX_TABLE, "inject_shutdown_error");
	if (!lua_isnil(L, -1))
		conf.inject_shutdown_error = lua_toboolean(L, -1);
	lua_pop(L, 1);
	return 0;

error_no_parameters:
	assert(lerr != NULL);
	return luaL_error(L, lerr);
}

/* Launched in TX thread.
 * Actually this function can be moved to another module. */
static int
debug_wait_process(lua_State *L)
{
	/* Lua parameters: PID as string. */
	enum {
		LUA_STACK_DEBUG_IDX_PID = 1,
		LUA_STACK_REQUIRED_PARAMS_COUNT = LUA_STACK_DEBUG_IDX_PID,
	};
	const char *lerr = NULL;
	if (lua_gettop(L) < LUA_STACK_REQUIRED_PARAMS_COUNT) {
		lerr = "No parameters specified";
		goto error_no_parameters;
	}
	size_t str_len;
	const char *const pid_str =
		lua_tolstring(L, LUA_STACK_DEBUG_IDX_PID, &str_len);
	if (pid_str == NULL) {
		lerr = "PID is not a string";
		goto error_pid_not_str;
	}

	char tmp[16];
	if (str_len >= sizeof(tmp)) {
		lerr = "PID is too long";
		goto error_pid_too_long;
	}
	memcpy(tmp, pid_str, str_len);
	unsigned pid;
	int count = sscanf(tmp, "%u", &pid);
	if (1 != count) {
		lerr = "Can't parse PID";
		goto error_cant_parse_pid;
	}

	process_helper_req_t req;
	req.type = TYPE_TRYWAIT;
	req.un.pid = pid;
retry_everything:
	;
	const int reaper_client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (reaper_client_fd < 0) {
		lerr = "socket() failed";
		goto error_cant_socket;
	}
	if (connect(reaper_client_fd, (struct sockaddr *)&reaper_addr,
	    sizeof(reaper_addr)) < 0) {
		lerr = "connect() to process_helper failed";
		goto error_cant_connect;
	}
	ssize_t bytes_already_sent;
retry_query:
	bytes_already_sent = 0;
retry_send:
	;
	const ssize_t bytes_sent = send(reaper_client_fd,
		(char *)&req + bytes_already_sent,
		sizeof(req) - bytes_already_sent, 0);
	if (bytes_sent < 0) {
		if (errno == EINTR)
			goto retry_send;
		if (errno == EPIPE || errno == ENOTCONN) {
			close(reaper_client_fd);
			goto retry_everything;
		}
		perror("send() to process_helper failed");
		lerr = "send() to process_helper failed";
		goto error_cant_send;
	}
	bytes_already_sent += bytes_sent;
	if (bytes_already_sent < sizeof(req))
		goto retry_send;

	pid_t code;
	/* FIXME: Handle EINTR at least? */
	if (recv(reaper_client_fd, &code, sizeof(code), 0) < sizeof(code)) {
		perror("recv() from process_helper failed");
		lerr = "recv() from process_helper failed";
		goto error_cant_recv;
	}
	if (code == TRYWAIT_RESULT_AGAIN) {
		fiber_sleep(0.001);
		goto retry_query;
	}

	close(reaper_client_fd);

	if (code < 0)
		return 0;

	lua_pushinteger(L, WEXITSTATUS(code));
	return 1;

error_cant_recv:
error_cant_send:
error_cant_connect:
	close(reaper_client_fd);
error_cant_socket:
error_cant_parse_pid:
error_pid_too_long:
error_pid_not_str:
error_no_parameters:
	assert(lerr != NULL);
	return luaL_error(L, lerr);
}

/* Launched in HTTP server thread. */
void *
get_router_data(shuttle_t *shuttle, unsigned *max_len)
{
	*max_len = conf.max_path_len_lua;
	lua_handler_state_t *const state =
		(lua_handler_state_t *)&shuttle->payload;
	return &state->un.req.buffer;
}

static const struct luaL_Reg mylib[] = {
	{"cfg", cfg},
	{"shutdown", on_shutdown_for_user},
#ifdef SUPPORT_RECONFIG
	{"force_decrease_threads", force_decrease_threads},
#endif /* SUPPORT_RECONFIG */
	{"_cfg_debug", cfg_debug},
	{"_debug_wait_process", debug_wait_process},
	{NULL, NULL}
};

int
luaopen_httpng_c(lua_State *L)
{
	/* Can't use "designated initializer" in C89 or C++. */
	reaper_addr.sun_family = AF_UNIX;
	memcpy(reaper_addr.sun_path, REAPER_SOCKET_NAME,
		sizeof(REAPER_SOCKET_NAME));

	box_null_cdata_type = luaL_ctypeid(L, "void *");

	luaL_newlib(L, mylib);
	(void)luaL_dostring(L, "return require 'httpng.router'");
	lua_setfield(L, -2, "router");
	return 1;
}
