#ifndef HTTPNG_CONFIG_H
#define HTTPNG_CONFIG_H

#define SUPPORT_SHUTDOWN
//#undef SUPPORT_SHUTDOWN
#define SUPPORT_CONFIGURING_OPENSSL
//#undef SUPPORT_CONFIGURING_OPENSSL
#define SUPPORT_WEBSOCKETS
//#undef SUPPORT_WEBSOCKETS
#define SUPPORT_THR_TERMINATION
//#undef SUPPORT_THR_TERMINATION
#define SUPPORT_RECONFIG
//#undef SUPPORT_RECONFIG
#define SUPPORT_LISTEN
//#undef SUPPORT_LISTEN
#define SUPPORT_C_HANDLERS
#undef SUPPORT_C_HANDLERS
#define SUPPORT_REQ_INFO
//#undef SUPPORT_REQ_INFO
#define SUPPORT_ROUTER
//#undef SUPPORT_ROUTER
#define SUPPORT_DEBUG_API
//#undef SUPPORT_DEBUG_API
#define SUPPORT_FULL_WRITE_API
//#undef SUPPORT_FULL_WRITE_API
#define SUPPORT_GRACEFUL_THR_TERMINATION
//#undef SUPPORT_GRACEFUL_THR_TERMINATION
#define SUPPORT_RETURN_FROM_WRITE
//#undef SUPPORT_RETURN_FROM_WRITE

/* When disabled, HTTP requests with body not fitting into shuttle are failed.
 * N. b.: h2o allocates memory for the WHOLE body in any case. */
#define SUPPORT_SPLITTING_LARGE_BODY
//#undef SUPPORT_SPLITTING_LARGE_BODY

#define SHOULD_FREE_SHUTTLE_IN_HTTP_SERVER_THREAD
#undef SHOULD_FREE_SHUTTLE_IN_HTTP_SERVER_THREAD

#ifdef SUPPORT_WEBSOCKETS
#define SHOULD_FREE_RECV_DATA_IN_HTTP_SERVER_THREAD
#undef SHOULD_FREE_RECV_DATA_IN_HTTP_SERVER_THREAD
#endif /* SUPPORT_WEBSOCKETS */

#ifndef SHOULD_FREE_SHUTTLE_IN_HTTP_SERVER_THREAD
#define USE_SHUTTLES_MUTEX
#endif /* SHOULD_FREE_SHUTTLE_IN_HTTP_SERVER_THREAD */

#ifdef SUPPORT_GRACEFUL_THR_TERMINATION
# ifndef SUPPORT_SHUTDOWN
#  error "SUPPORT_GRACEFUL_THR_TERMINATION requires SUPPORT_SHUTDOWN"
# endif /* SUPPORT_SHUTDOWN */
#endif /* SUPPORT_GRACEFUL_THR_TERMINATION */

#ifdef USE_LIBUV
#define H2O_USE_LIBUV 1
#else
#define H2O_USE_LIBUV 0
#endif /* USE_LIBUV */

#endif /* HTTPNG_CONFIG_H */
