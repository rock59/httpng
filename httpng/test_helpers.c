#include "process_helper.h"
#include <assert.h>
#include <errno.h>
#include <lauxlib.h>
#include <module.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static struct sockaddr_un reaper_addr;

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
	if (bytes_already_sent < (ssize_t)sizeof(req))
		goto retry_send;

	pid_t code;
	/* FIXME: Handle EINTR at least? */
	if (recv(reaper_client_fd, &code, sizeof(code), 0) <
	    (ssize_t)sizeof(code)) {
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

static const struct luaL_Reg mylib[] = {
	{"debug_wait_process", debug_wait_process},
	{NULL, NULL}
};

int
luaopen_test_helpers(lua_State *L)
{
	/* Can't use "designated initializer" in C89 or C++. */
	reaper_addr.sun_family = AF_UNIX;
	memcpy(reaper_addr.sun_path, REAPER_SOCKET_NAME,
		sizeof(REAPER_SOCKET_NAME));

	luaL_newlib(L, mylib);
	return 1;
}
