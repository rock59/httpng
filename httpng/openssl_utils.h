#ifndef OPENSSL_UTILS_H
#define OPENSSL_UTILS_H

#include <openssl/ssl.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

SSL_CTX *make_ssl_ctx(const char *certificate_file, const char *key_file,
	int security_level, long min_proto_version, const char **lerr);

#ifdef __cplusplus
} /* extern "C" */
#endif /* __cplusplus */
#endif /* OPENSSL_UTILS_H */
