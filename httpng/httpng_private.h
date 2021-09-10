#ifndef HTTPNG_PRIVATE_H
#define HTTPNG_PRIVATE_H

#include <xtm/src/xtm_api.h>
#include "httpng_sem.h"
#include "httpng_config.h"
#include <h2o.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#define DEFAULT_threads 1
#define DEFAULT_max_conn_per_thread (256 * 1024)
#define DEFAULT_max_shuttles_per_thread 4096
#define DEFAULT_shuttle_size 65536

/* Limits are quite relaxed for now. */
#define MIN_threads 1
#define MIN_max_conn_per_thread 1
#define MIN_max_shuttles_per_thread 1
#define MIN_shuttle_size (sizeof(shuttle_t) + sizeof(uintptr_t))

/* Limits are quite relaxed for now. */
#define MAX_threads 16 /* More than 4 is hardly useful (Lua). */
#define MAX_max_conn_per_thread (256 * 1024 * 1024)
#define MAX_max_shuttles_per_thread UINT_MAX
#define MAX_shuttle_size (16 * 1024 * 1024)

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

typedef unsigned shuttle_count_t;

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
	waiter_t *call_from_tx_waiter;
	h2o_hostconf_t *hostconf;
	/* For xtm; it is probably not socket underneath. */
	h2o_socket_t *sock_from_tx;
	h2o_linklist_t accepted_sockets;
	shuttle_count_t shuttle_counter;
	unsigned num_connections;
	unsigned idx;
	unsigned listeners_created;
	pthread_t tid;
	bool push_from_tx_is_sleeping;
	bool queue_from_tx_fd_consumed;
} thread_ctx_t;

struct anchor;
typedef struct shuttle {
	h2o_req_t *never_access_this_req_from_tx_thread;
	struct anchor *anchor;
	thread_ctx_t *thread_ctx;

	/* never_access_this_req_from_tx_thread can only be used
	 * if disposed is false. */
	char disposed;

	char unused[sizeof(void *) - 1 * sizeof(char)];

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
	h2o_socket_t *sock;
	int fd;
} listener_ctx_t;

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
	int lua_handler_ref; /* Reference to user Lua handler. */
	unsigned router_data_len;
	unsigned path_len;
	unsigned authority_len;
	unsigned query_at;
	unsigned num_headers;
	unsigned body_len;
	unsigned char method_len;
	unsigned char version_major;
	unsigned char version_minor;
	char method[7];
	char buffer[];
} lua_first_request_only_t;

typedef struct {
	struct fiber *fiber;
	waiter_t *waiter; /* Changed by TX thread. */
	int lua_state_ref;
	bool fiber_done;
	bool sent_something;
	bool cancelled; /* Changed by TX thread. */

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
	lua_h2o_handler_t *lua_handler;
	uint64_t openssl_security_level;
	long min_tls_proto_version;
	shuttle_count_t max_shuttles_per_thread;
	unsigned shuttle_size;
	unsigned num_listeners;
	unsigned num_accepts;
	unsigned max_conn_per_thread;
	unsigned num_threads;
	unsigned max_headers_lua;
	unsigned max_path_len_lua;
	int lua_handler_ref;
	int tfo_queues;
	bool configured;
	bool cfg_in_progress;
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
shuttle_t *prepare_shuttle(h2o_req_t *);

/* Launched in HTTP server thread.
 * FIXME: Rename after changing sample C handlers. */
static inline void
free_shuttle(shuttle_t *shuttle)
{
	thread_ctx_t *const thread_ctx = shuttle->thread_ctx;
	--thread_ctx->shuttle_counter;
	free(shuttle);
}

/* Can be launched in TX thread or HTTP(S) server thread. */
static inline shuttle_t *
get_shuttle(lua_handler_state_t *state)
{
	return (shuttle_t *)((char *)state - offsetof(shuttle_t, payload));
}

#ifdef __cplusplus
} /* extern "C" */
#endif /* __cplusplus */
#endif /* HTTPNG_PRIVATE_H */
