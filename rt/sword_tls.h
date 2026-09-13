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
//
// A client may also present a certificate of its own — `cert_file` and `key_file`,
// both empty when it does not — which is what a server asking "who are you" checks.
void *sword_tls_client_context(const char *ca_file, int64_t ca_len,
                               int32_t verify, const char *cert_file,
                               int64_t cert_len, const char *key_file,
                               int64_t key_len);
// `ca_file` empty asks nothing of the client. Given one, `require` decides whether a
// client that presents nothing, or something signed by somebody else, is turned away
// (1) or let in unauthenticated (0). Those are different servers and the difference
// is the whole point of asking.
void *sword_tls_server_context(const char *cert_file, int64_t cert_len,
                               const char *key_file, int64_t key_len,
                               const char *ca_file, int64_t ca_len,
                               int32_t require);
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

// The subject of the certificate the other end presented, written into `out`, or -1
// when it presented none. A server that asks for one needs to know who answered:
// authenticating and then not looking is the same as not asking.
int64_t sword_tls_peer_name(void *conn, char *out, int64_t out_len);
// 1 when the other end's certificate was presented and verified, 0 otherwise.
int32_t sword_tls_peer_verified(void *conn);

// Writes a certificate and its key to two paths, signed by nobody, valid for a
// day, for a name of your choosing. For development and for tests: a server with
// one of these is a server no client should trust without being told to.
int32_t sword_tls_self_signed(const char *host, int64_t host_len,
                              const char *cert_file, int64_t cert_len,
                              const char *key_file, int64_t key_len);

// A certificate for `name`, signed by the key and certificate in `ca_*` rather than
// by its own. Two of these from one authority are two identities that can check each
// other, which is what a test of mutual TLS needs and what a private authority does
// for real. `client` marks it for client authentication rather than server.
int32_t sword_tls_signed_by(const char *name, int64_t name_len,
                            const char *ca_cert, int64_t ca_cert_len,
                            const char *ca_key, int64_t ca_key_len,
                            const char *cert_file, int64_t cert_len,
                            const char *key_file, int64_t key_len,
                            int32_t client);
}
