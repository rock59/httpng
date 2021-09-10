#include "openssl_utils.h"
#include "httpng_config.h"
#include <h2o.h> /* For #if's */
#include <assert.h>

/* NOTE: only PEM format is allowed */
SSL_CTX *
make_ssl_ctx(const char *certificate_file, const char *key_file,
	     int level, long min_proto_version, const char **lerr)
{
	SSL_CTX *ssl_ctx = SSL_CTX_new(SSLv23_server_method());
	if (ssl_ctx == NULL) {
		*lerr = "memory allocation for ssl context failed";
		goto make_ssl_ctx_error;
	}

	SSL_CTX_set_min_proto_version(ssl_ctx, min_proto_version);

	SSL_CTX_set_security_level(ssl_ctx, level);

	if (certificate_file != NULL && key_file != NULL) {
		if (SSL_CTX_use_certificate_chain_file(ssl_ctx,
		    certificate_file) != 1) {
			*lerr = "can't bind certificate file to ssl_ctx";
			goto make_ssl_ctx_error;
		}
		if (SSL_CTX_use_PrivateKey_file(ssl_ctx,
		    key_file, SSL_FILETYPE_PEM) != 1) {
			*lerr = "can't bind private key to ssl_ctx";
			goto make_ssl_ctx_error;
		}
		if (SSL_CTX_check_private_key(ssl_ctx) != 1) {
			*lerr = "check private key with certificate failed";
			goto make_ssl_ctx_error;
		}
	}

/* Setup protocol negotiation methods. */
#if H2O_USE_NPN
	h2o_ssl_register_npn_protocols(ssl_ctx, h2o_http2_npn_protocols);
#endif /* H2O_USE_NPN */
#if H2O_USE_ALPN
#ifdef DISABLE_HTTP2
	/* Disable HTTP/2 e. g. to test WebSockets. */
	static const h2o_iovec_t my_alpn_protocols[] = {
		{H2O_STRLIT("http/1.1")}, {NULL}
	};
	h2o_ssl_register_alpn_protocols(ssl_ctx, my_alpn_protocols);
#else /* DISABLE_HTTP2 */
	h2o_ssl_register_alpn_protocols(ssl_ctx, h2o_http2_alpn_protocols);
#endif /* DISABLE_HTTP2 */
#endif /* H2O_USE_ALPN */

	return ssl_ctx;

make_ssl_ctx_error:
	assert(*lerr != NULL);
	if (ssl_ctx != NULL)
		SSL_CTX_free(ssl_ctx);
	return NULL;
}
