#ifndef HTTPNG_ROUTER_H
#define HTTPNG_ROUTER_H

#ifdef USE_LIBUV
#define H2O_USE_LIBUV 1
#else
#define H2O_USE_EPOLL 1 /* FIXME */
#include <h2o/evloop_socket.h>
#endif /* USE_LIBUV */

#include <h2o.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#define ROUTER_C_HANDLER_FIELD_NAME "_handle_request"
#define ROUTER_C_HANDLER_PARAM_FIELD_NAME "_handle_request_param"
#define ROUTER_FILL_ROUTER_DATA_FIELD_NAME "_fill_router_data"

/* FIXME: Make it ushort and add sanity checks. */
typedef unsigned header_offset_t;

typedef struct {
	h2o_handler_t super;
	int lua_handler_ref;
	int router_ref; /* Not used for C routers. */

	/* FIXME: Use separate struct for router handler? */
	void *c_handler_param;
} lua_h2o_handler_t;

struct lua_State;
typedef void (fill_router_data_t)(struct lua_State *L, const char *path,
	unsigned data_len, void *data);

struct shuttle;
struct shuttle *prepare_shuttle2(h2o_req_t *);
int lua_req_handler_ex(h2o_req_t *,
	struct shuttle *, int, unsigned, int);
void *get_router_data(struct shuttle *, unsigned *);
void httpng_flush_requests_to_router(void);

/* Must be called in HTTP server thread.
 * Should only be called if disposed==false.
 * Expected usage: when req handler can't or wouldn't queue request
 * to TX thread. */
extern void free_shuttle_with_anchor(struct shuttle *);

#ifdef __cplusplus
} /* extern "C" */
#endif /* __cplusplus */

#endif /* HTTPNG_ROUTER_H */
