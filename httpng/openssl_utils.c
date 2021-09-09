#include "openssl_utils.h"
#include "httpng_config.h"
#include <h2o.h> /* For #if's */
#include <assert.h>

#ifdef SUPPORT_LISTEN
/* NOTE: only PEM format certificate is allowed */
X509 *
get_X509_from_certificate_path(const char *cert_path, const char **lerr)
{
	X509 *cert = NULL;
	FILE *fp = NULL;

	assert(cert_path != NULL);
	assert(lerr != NULL);

	fp = fopen(cert_path, "r");
	if (fp == NULL) {
		*lerr = "unable to open certificate file";
		goto fopen_fail;
	}
	cert = PEM_read_X509(fp, NULL, NULL, NULL);
	if (cert == NULL) {
		*lerr = "unable to parse certificate: only PEM format is allowed";
		goto read_X509_fail;
	}

read_X509_fail:
	fclose(fp);
fopen_fail:
	assert(cert != NULL || *lerr != NULL);
	return cert;
}
#endif /* SUPPORT_LISTEN */

#ifdef SUPPORT_LISTEN
const char *
get_subject_common_name(X509 *cert)
{
	assert(cert != NULL);
	X509_NAME *subj = X509_get_subject_name(cert);

	int length = X509_NAME_get_text_by_NID(subj, NID_commonName, NULL, 0);
	char *common_name = calloc(length + 1, sizeof(*common_name));
	if (common_name == NULL)
		return NULL;
	int retval = X509_NAME_get_text_by_NID(subj, NID_commonName,
		common_name, length + 1);
	if (retval < 0) {
		free(common_name);
		return NULL;
	}
	return common_name;
}
#endif /* SUPPORT_LISTEN */

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
