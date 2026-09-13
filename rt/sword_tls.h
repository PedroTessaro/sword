#pragma once

#include <stdint.h>

// TLS over a socket the rest of the runtime already owns. The protocol is not
// something to write by hand, so this is a thin layer over OpenSSL: it holds the
// non-blocking handshake and the read and write loops, which are the parts that
// have to know about tasks. Everything above it is written in Sword.
//
// Built without OpenSSL, every call here fails and `sword_tls_available` answers
// zero — a program that never asks for TLS is unaffected either way.
extern "C" {

int32_t sword_tls_available(void);

// A context is the certificate, the key and the trust settings; one serves any
// number of connections. Null on failure. `verify` off is for talking to a
// development server with a certificate nobody signed.
void *sword_tls_client_context(const char *ca_file, int64_t ca_len,
                               int32_t verify);
void *sword_tls_server_context(const char *cert_file, int64_t cert_len,
                               const char *key_file, int64_t key_len);
void sword_tls_free_context(void *ctx);

// Takes over a connected socket and handshakes on it, as the side that dialled
// or the side that accepted. The host is what the certificate is checked against
// and what SNI carries. Null when the handshake failed.
void *sword_tls_connect(void *ctx, int32_t fd, const char *host,
                        int64_t host_len);
void *sword_tls_accept(void *ctx, int32_t fd);

// Bytes moved, 0 at a clean end of stream, -1 on failure, -2 out of time. Both
// put the task down while the socket is not ready, the same as a plain read.
int64_t sword_tls_read(void *conn, void *buf, int64_t len);
int64_t sword_tls_write(void *conn, const void *buf, int64_t len);

// Sends the close notification, best effort, and releases the connection. The
// socket itself belongs to the caller.
void sword_tls_close(void *conn);

// The protocol version that was agreed, as a string ("TLSv1.3"), and the cipher.
// Empty when there is no connection.
const char *sword_tls_version(void *conn, int64_t *len);
const char *sword_tls_cipher(void *conn, int64_t *len);

// Writes a certificate and its key to two paths, signed by nobody, valid for a
// day, for a name of your choosing. For development and for tests: a server with
// one of these is a server no client should trust without being told to.
int32_t sword_tls_self_signed(const char *host, int64_t host_len,
                              const char *cert_file, int64_t cert_len,
                              const char *key_file, int64_t key_len);
}
