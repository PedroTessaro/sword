#include "sword_tls.h"

#include "sword_net.h"
#include "sword_rt.h"

#include <stdio.h>
#include <string.h>

#ifndef SWORD_HAVE_TLS

// No OpenSSL at build time. Every entry point says so rather than pretending,
// and nothing else in the runtime changes.
extern "C" {

int32_t sword_tls_available(void) { return 0; }

void *sword_tls_client_context(const char *, int64_t, int32_t) {
  return nullptr;
}
void *sword_tls_server_context(const char *, int64_t, const char *, int64_t) {
  return nullptr;
}
void sword_tls_free_context(void *) {}
void *sword_tls_connect(void *, int32_t, const char *, int64_t) {
  return nullptr;
}
void *sword_tls_accept(void *, int32_t) { return nullptr; }
int64_t sword_tls_read(void *, void *, int64_t) { return -1; }
int64_t sword_tls_write(void *, const void *, int64_t) { return -1; }
void sword_tls_close(void *) {}
const char *sword_tls_version(void *, int64_t *len) {
  *len = 0;
  return "";
}
const char *sword_tls_cipher(void *, int64_t *len) {
  *len = 0;
  return "";
}
int32_t sword_tls_self_signed(const char *, int64_t, const char *, int64_t,
                              const char *, int64_t) {
  return -1;
}
}

#else

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

namespace {

// A Sword string is not terminated and OpenSSL wants a C string.
bool as_path(const char *text, int64_t len, char *into, size_t room) {
  if (len <= 0 || (size_t)len >= room) return false;
  memcpy(into, text, (size_t)len);
  into[len] = '\0';
  return true;
}

struct Conn {
  SSL *ssl;
  int fd;
};

// The library initialises itself on first use in OpenSSL 3, so all this has to
// do is make sure it happens once.
void ensure_library(void) {
  static bool done = [] {
    SSL_library_init();
    return true;
  }();
  (void)done;
}

// What OpenSSL wants next. A socket that is not ready is not a failure: the task
// is put down until it is, which is the whole reason this layer exists rather
// than the Sword side driving OpenSSL directly.
//
// Returns 1 to try the call again, 0 for a clean end, -1 for failure, -2 for out
// of time.
int settle(Conn *c, int result) {
  int reason = SSL_get_error(c->ssl, result);
  switch (reason) {
  case SSL_ERROR_WANT_READ: {
    int ready = sword_net_await(c->fd, 0);
    return ready == 0 ? 1 : (ready == -2 ? -2 : -1);
  }
  case SSL_ERROR_WANT_WRITE: {
    int ready = sword_net_await(c->fd, 1);
    return ready == 0 ? 1 : (ready == -2 ? -2 : -1);
  }
  case SSL_ERROR_ZERO_RETURN:
    return 0; // the peer said goodbye properly
  case SSL_ERROR_SYSCALL:
    // A peer that vanished mid-stream. Common enough on the open internet that
    // it is not worth a distinct answer.
    return result == 0 ? 0 : -1;
  default:
    ERR_clear_error();
    return -1;
  }
}

// Handshakes until it is done or it is not going to be. `as_client` decides which
// half of the protocol OpenSSL drives.
bool handshake(Conn *c, bool as_client) {
  while (true) {
    ERR_clear_error();
    int result = as_client ? SSL_connect(c->ssl) : SSL_accept(c->ssl);
    if (result == 1) return true;
    if (settle(c, result) != 1) return false;
  }
}

Conn *wrap(void *ctx, int fd, const char *host, int64_t host_len,
           bool as_client) {
  if (!ctx) return nullptr;
  ensure_library();

  SSL *ssl = SSL_new((SSL_CTX *)ctx);
  if (!ssl) return nullptr;
  if (SSL_set_fd(ssl, fd) != 1) {
    SSL_free(ssl);
    return nullptr;
  }

  if (as_client && host_len > 0) {
    char name[256];
    if (as_path(host, host_len, name, sizeof(name))) {
      // SNI, so a server with several certificates knows which to send, and the
      // name the certificate is checked against. Both, or verification passes on
      // a certificate for somebody else.
      SSL_set_tlsext_host_name(ssl, name);
      SSL_set1_host(ssl, name);
    }
  }

  Conn *c = new Conn{ssl, fd};
  if (!handshake(c, as_client)) {
    SSL_free(ssl);
    delete c;
    return nullptr;
  }
  return c;
}

} // namespace

extern "C" {

int32_t sword_tls_available(void) { return 1; }

void *sword_tls_client_context(const char *ca_file, int64_t ca_len,
                               int32_t verify) {
  ensure_library();
  SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
  if (!ctx) return nullptr;
  // 1.2 is the floor: everything below it is broken in public, and nothing that
  // still runs needs it.
  SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

  if (verify != 0) {
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
    char path[1024];
    if (ca_len > 0 && as_path(ca_file, ca_len, path, sizeof(path))) {
      if (SSL_CTX_load_verify_locations(ctx, path, nullptr) != 1) {
        SSL_CTX_free(ctx);
        return nullptr;
      }
    } else if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
      SSL_CTX_free(ctx);
      return nullptr;
    }
  } else {
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
  }
  return ctx;
}

void *sword_tls_server_context(const char *cert_file, int64_t cert_len,
                               const char *key_file, int64_t key_len) {
  ensure_library();
  char cert[1024];
  char key[1024];
  if (!as_path(cert_file, cert_len, cert, sizeof(cert))) return nullptr;
  if (!as_path(key_file, key_len, key, sizeof(key))) return nullptr;

  SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
  if (!ctx) return nullptr;
  SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
  if (SSL_CTX_use_certificate_chain_file(ctx, cert) != 1 ||
      SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM) != 1 ||
      SSL_CTX_check_private_key(ctx) != 1) {
    ERR_clear_error();
    SSL_CTX_free(ctx);
    return nullptr;
  }
  return ctx;
}

void sword_tls_free_context(void *ctx) {
  if (ctx) SSL_CTX_free((SSL_CTX *)ctx);
}

void *sword_tls_connect(void *ctx, int32_t fd, const char *host,
                        int64_t host_len) {
  return wrap(ctx, (int)fd, host, host_len, true);
}

void *sword_tls_accept(void *ctx, int32_t fd) {
  return wrap(ctx, (int)fd, nullptr, 0, false);
}

int64_t sword_tls_read(void *conn, void *buf, int64_t len) {
  Conn *c = (Conn *)conn;
  if (!c || len <= 0) return -1;
  while (true) {
    ERR_clear_error();
    int got = SSL_read(c->ssl, buf, (int)(len > INT32_MAX ? INT32_MAX : len));
    if (got > 0) return got;
    int next = settle(c, got);
    if (next != 1) return next;
  }
}

// All of it or nothing, the same contract a socket write has here. OpenSSL may
// take a partial buffer, so this is a loop rather than a call.
int64_t sword_tls_write(void *conn, const void *buf, int64_t len) {
  Conn *c = (Conn *)conn;
  if (!c) return -1;
  int64_t sent = 0;
  while (sent < len) {
    int64_t left = len - sent;
    ERR_clear_error();
    int put = SSL_write(c->ssl, (const char *)buf + sent,
                        (int)(left > INT32_MAX ? INT32_MAX : left));
    if (put > 0) {
      sent += put;
      continue;
    }
    int next = settle(c, put);
    if (next != 1) return next == 0 ? sent : next;
  }
  return sent;
}

void sword_tls_close(void *conn) {
  Conn *c = (Conn *)conn;
  if (!c) return;
  // One attempt at a clean goodbye. Waiting for the peer's half would hold the
  // task on a socket that may never answer, and the record after it is the
  // close itself.
  ERR_clear_error();
  SSL_shutdown(c->ssl);
  SSL_free(c->ssl);
  delete c;
}

const char *sword_tls_version(void *conn, int64_t *len) {
  Conn *c = (Conn *)conn;
  const char *text = c ? SSL_get_version(c->ssl) : "";
  if (!text) text = "";
  *len = (int64_t)strlen(text);
  return text;
}

const char *sword_tls_cipher(void *conn, int64_t *len) {
  Conn *c = (Conn *)conn;
  const char *text = c ? SSL_get_cipher_name(c->ssl) : "";
  if (!text) text = "";
  *len = (int64_t)strlen(text);
  return text;
}

// A certificate for `host`, signed by its own key. Enough to talk TLS to
// yourself, which is what a development server and a test need, and worth
// nothing to anybody else — no client trusts this without being told to.
int32_t sword_tls_self_signed(const char *host, int64_t host_len,
                              const char *cert_file, int64_t cert_len,
                              const char *key_file, int64_t key_len) {
  ensure_library();
  char name[256];
  char cert_path[1024];
  char key_path[1024];
  if (!as_path(host, host_len, name, sizeof(name))) return -1;
  if (!as_path(cert_file, cert_len, cert_path, sizeof(cert_path))) return -1;
  if (!as_path(key_file, key_len, key_path, sizeof(key_path))) return -1;

  EVP_PKEY *key = EVP_RSA_gen(2048);
  if (!key) return -1;

  X509 *cert = X509_new();
  if (!cert) {
    EVP_PKEY_free(key);
    return -1;
  }

  int ok = 0;
  do {
    X509_set_version(cert, 2); // v3, which is what a SAN needs
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
    X509_gmtime_adj(X509_getm_notBefore(cert), 0);
    X509_gmtime_adj(X509_getm_notAfter(cert), 60 * 60 * 24);
    if (X509_set_pubkey(cert, key) != 1) break;

    X509_NAME *subject = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                               (const unsigned char *)name, -1, -1, 0);
    X509_set_issuer_name(cert, subject); // its own issuer: that is the point

    // Modern clients check the subject alternative name and ignore the common
    // name, so without this the certificate is for nobody.
    char alt[320];
    bool numeric = name[0] >= '0' && name[0] <= '9';
    snprintf(alt, sizeof(alt), numeric ? "IP:%s" : "DNS:%s", name);
    X509_EXTENSION *san = X509V3_EXT_conf_nid(nullptr, nullptr,
                                              NID_subject_alt_name, alt);
    if (san) {
      X509_add_ext(cert, san, -1);
      X509_EXTENSION_free(san);
    }

    if (X509_sign(cert, key, EVP_sha256()) == 0) break;

    FILE *out = fopen(cert_path, "wb");
    if (!out) break;
    int wrote = PEM_write_X509(out, cert);
    fclose(out);
    if (wrote != 1) break;

    out = fopen(key_path, "wb");
    if (!out) break;
    wrote = PEM_write_PrivateKey(out, key, nullptr, nullptr, 0, nullptr,
                                 nullptr);
    fclose(out);
    if (wrote != 1) break;
    ok = 1;
  } while (false);

  X509_free(cert);
  EVP_PKEY_free(key);
  if (!ok) ERR_clear_error();
  return ok ? 0 : -1;
}
}

#endif
