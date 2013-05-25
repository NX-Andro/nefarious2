/************************************************************************
 *   IRC - Internet Relay Chat, ircd/ssl.c
 *   Copyright (C) 2002 Alex Badea <vampire@go.ro>
 *   Copyright (C) 2013 Matthew Beeching (Jobe)
 *
 *   See file AUTHORS in IRC package for additional names of
 *   the programmers.
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 1, or (at your option)
 *   any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
/** @file
 * @brief Implimentation of common SSL functions.
 * @version $Id:$
 */
#include "config.h"
#include "client.h"
#include "ircd_alloc.h"
#include "ircd_features.h"
#include "ircd_log.h"
#include "ircd_string.h"
#include "listener.h"
#include "s_debug.h"
#include "ssl.h"

#ifdef USE_SSL

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/uio.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <openssl/rand.h>
#include <openssl/ssl.h>

struct ssl_data {
  struct Socket socket;
  struct Listener *listener;
  int fd;
};

SSL_CTX *ssl_server_ctx;
SSL_CTX *ssl_client_ctx;

SSL_CTX *ssl_init_server_ctx();
SSL_CTX *ssl_init_client_ctx();
int ssl_verify_callback(int preverify_ok, X509_STORE_CTX *cert);
void sslfail(char *txt);
void binary_to_hex(unsigned char *bin, char *hex, int length);

int ssl_init(void)
{
  SSL_library_init();
  SSL_load_error_strings();

  Debug((DEBUG_NOTICE, "SSL: read %d bytes of randomness", RAND_load_file("/dev/urandom", 4096)));

  ssl_server_ctx = ssl_init_server_ctx();
  if (!ssl_server_ctx)
    return -1;
  ssl_client_ctx = ssl_init_client_ctx();
  if (!ssl_client_ctx)
    return -1;

  return 0;
}

int ssl_reinit(void)
{
  SSL_CTX *temp_ctx;

  /* Attempt to reinitialize server context, return on error */
  temp_ctx = ssl_init_server_ctx();
  if (!temp_ctx)
    return -1;

  /* Now reinitialize server context for real. */
  SSL_CTX_free(temp_ctx);
  SSL_CTX_free(ssl_server_ctx);
  ssl_server_ctx = ssl_init_server_ctx();

  /* Attempt to reinitialize client context, return on error */
  temp_ctx = ssl_init_client_ctx();
  if (!temp_ctx)
    return -1;

  /* Now reinitialize client context for real. */
  SSL_CTX_free(temp_ctx);
  SSL_CTX_free(ssl_client_ctx);
  ssl_client_ctx = ssl_init_client_ctx();

  return 0;
}

SSL_CTX *ssl_init_server_ctx(void)
{
  SSL_CTX *server_ctx = NULL;

  server_ctx = SSL_CTX_new(SSLv23_server_method());
  if (!server_ctx)
  {
    sslfail("Error creating new server context");
    return NULL;
  }

  //SSL_CTX_set_options(server_ctx, SSL_OP_NO_SSLv2);
  SSL_CTX_set_verify(server_ctx, SSL_VERIFY_PEER|SSL_VERIFY_CLIENT_ONCE, ssl_verify_callback);
  SSL_CTX_set_session_cache_mode(server_ctx, SSL_SESS_CACHE_OFF);

  if (SSL_CTX_use_certificate_chain_file(server_ctx, feature_str(FEAT_SSL_CERTFILE)) <= 0)
  {
    sslfail("Error loading SSL certificate for server context");
    SSL_CTX_free(server_ctx);
    return NULL;
  }
  if (SSL_CTX_use_PrivateKey_file(server_ctx, feature_str(FEAT_SSL_KEYFILE), SSL_FILETYPE_PEM) <= 0)
  {
    sslfail("Error loading SSL key for server context");
    SSL_CTX_free(server_ctx);
    return NULL;
  }

  return server_ctx;
}

SSL_CTX *ssl_init_client_ctx(void)
{
  SSL_CTX *client_ctx = NULL;

  client_ctx = SSL_CTX_new(SSLv3_client_method());
  if (!client_ctx)
  {
    sslfail("Error creating new client context");
    return NULL;
  }

  SSL_CTX_set_session_cache_mode(client_ctx, SSL_SESS_CACHE_OFF);

  if (SSL_CTX_use_certificate_chain_file(client_ctx, feature_str(FEAT_SSL_CERTFILE)) <= 0)
  {
    sslfail("Error loading SSL certificate for client context");
    SSL_CTX_free(client_ctx);
    return NULL;
  }
  if (SSL_CTX_use_PrivateKey_file(client_ctx, feature_str(FEAT_SSL_KEYFILE), SSL_FILETYPE_PEM) <= 0)
  {
    sslfail("Error loading SSL key for client context");
    SSL_CTX_free(client_ctx);
    return NULL;
  }

  return client_ctx;
}

int ssl_verify_callback(int preverify_ok, X509_STORE_CTX *cert)
{
  return 1;
}

static void ssl_abort(struct ssl_data *data)
{
  Debug((DEBUG_DEBUG, "SSL: aborted"));
  SSL_free(data->socket.ssl);
  close(data->fd);
  socket_del(&data->socket);
}

static void ssl_accept(struct ssl_data *data)
{
  const char* const error_ssl = "ERROR :SSL connection error\r\n";

  if (SSL_accept(data->socket.ssl) <= 0) {
    unsigned long err = ERR_get_error();
    char string[120];

    if (err) {
      ERR_error_string(err, string);
      Debug((DEBUG_ERROR, "SSL_accept: %s", string));

      write(data->fd, error_ssl, strlen(error_ssl));

      ssl_abort(data);
    }
    return;
  }
  if (SSL_is_init_finished(data->socket.ssl)) {
    socket_del(&data->socket);
    add_connection(data->listener, data->fd, data->socket.ssl);
  }
}

static void ssl_sock_callback(struct Event* ev)
{
  struct ssl_data *data;

  assert(0 != ev_socket(ev));
  assert(0 != s_data(ev_socket(ev)));

  data = s_data(ev_socket(ev));
  assert(0 != data);

  switch (ev_type(ev)) {
  case ET_DESTROY:
    --data->listener->ref_count;
    MyFree(data);
    return;
  case ET_ERROR:
  case ET_EOF:
    ssl_abort(data);
    break;
  case ET_READ:
  case ET_WRITE:
    ssl_accept(data);
    break;
  default:
    break;
  }
}

void ssl_add_connection(struct Listener *listener, int fd)
{
  struct ssl_data *data;

  assert(0 != listener);

  if (!os_set_nonblocking(fd)) {
    close(fd);
    return;
  }
  os_disable_options(fd);

  data = (struct ssl_data *)MyMalloc(sizeof(struct ssl_data));
  data->listener = listener;
  data->fd = fd;
  if (!socket_add(&data->socket, ssl_sock_callback, (void *) data, SS_CONNECTED, SOCK_EVENT_READABLE, fd)) {
    close(fd);
    return;
  }
  if (!(data->socket.ssl = SSL_new(ssl_server_ctx))) {
    Debug((DEBUG_DEBUG, "SSL_new failed"));
    close(fd);
    return;
  }
  SSL_set_fd(data->socket.ssl, fd);
  ++listener->ref_count;
}

/*
 * ssl_recv - non blocking read of a connection
 * returns:
 *  1  if data was read or socket is blocked (recoverable error)
 *    count_out > 0 if data was read
 *
 *  0  if socket closed from other end
 *  -1 if an unrecoverable error occurred
 */
IOResult ssl_recv(struct Socket *socketh, char* buf,
                 unsigned int length, unsigned int* count_out)
{
  int res;

  assert(0 != socketh);
  assert(0 != buf);
  assert(0 != count_out);

  *count_out = 0;
  errno = 0;

  res = SSL_read(socketh->ssl, buf, length);
  switch (SSL_get_error(socketh->ssl, res)) {
  case SSL_ERROR_NONE:
    *count_out = (unsigned) res;
    return IO_SUCCESS;
  case SSL_ERROR_WANT_WRITE:
  case SSL_ERROR_WANT_READ:
  case SSL_ERROR_WANT_X509_LOOKUP:
    Debug((DEBUG_DEBUG, "SSL_read returned WANT_ - retrying"));
    return IO_BLOCKED;
  case SSL_ERROR_SYSCALL:
    if (res < 0 && errno == EINTR)
      return IO_BLOCKED; /* ??? */
    break;
  case SSL_ERROR_ZERO_RETURN: /* close_notify received */
    SSL_shutdown(socketh->ssl); /* Send close_notify back */
    break;
  }
  return IO_FAILURE;
}

/*
 * ssl_sendv - non blocking writev to a connection
 * returns:
 *  1  if data was written
 *    count_out contains amount written
 *
 *  0  if write call blocked, recoverable error
 *  -1 if an unrecoverable error occurred
 */
IOResult ssl_sendv(struct Socket *socketh, struct MsgQ* buf,
                  unsigned int* count_in, unsigned int* count_out)
{
  int res;
  int count;
  int k;
  struct iovec iov[IOV_MAX];
  IOResult retval = IO_BLOCKED;
  int ssl_err = 0;

  errno = 0;

  assert(0 != socketh);
  assert(0 != buf);
  assert(0 != count_in);
  assert(0 != count_out);

  *count_in = 0;
  *count_out = 0;

  count = msgq_mapiov(buf, iov, IOV_MAX, count_in);
  for (k = 0; k < count; k++) {
    res = SSL_write(socketh->ssl, iov[k].iov_base, iov[k].iov_len);
    ssl_err = SSL_get_error(socketh->ssl, res);
    Debug((DEBUG_DEBUG, "SSL_write returned %d, error code %d.", res, ssl_err));
    switch (ssl_err) {
    case SSL_ERROR_NONE:
      *count_out += (unsigned) res;
      retval = IO_SUCCESS;
      break;
    case SSL_ERROR_WANT_WRITE:
    case SSL_ERROR_WANT_READ:
    case SSL_ERROR_WANT_X509_LOOKUP:
      Debug((DEBUG_DEBUG, "SSL_write returned want WRITE, READ, or X509; returning retval %d", retval));
      return retval;
    case SSL_ERROR_SSL:
      {
          int errorValue;
          Debug((DEBUG_ERROR, "SSL_write returned SSL_ERROR_SSL, errno %d, retval %d, res %d, ssl error code %d", errno, retval, res, ssl_err));
          ERR_load_crypto_strings();
          while((errorValue = ERR_get_error())) {
            Debug((DEBUG_ERROR, "  Error Queue: %d -- %s", errorValue, ERR_error_string(errorValue, NULL)));
          }
          return IO_FAILURE;
       }
    case SSL_ERROR_SYSCALL:
      if(res < 0 && (errno == EWOULDBLOCK ||
                     errno == EINTR ||
                     errno == EBUSY ||
                     errno == EAGAIN)) {
             Debug((DEBUG_DEBUG, "SSL_write returned ERROR_SYSCALL, errno %d - returning retval %d", errno, retval));
             return retval;
      }
      else {
             Debug((DEBUG_DEBUG, "SSL_write returned ERROR_SYSCALL - errno %d - returning IO_FAILURE", errno));
             return IO_FAILURE;
      }
      /*
      if(errno == EAGAIN) * its what unreal ircd does..*
      {
          Debug((DEBUG_DEBUG, "SSL_write returned ERROR_SSL - errno %d returning retval %d", errno, retval));
          return retval;
      }
      */
    case SSL_ERROR_ZERO_RETURN:
      SSL_shutdown(socketh->ssl);
      return IO_FAILURE;
    default:
      Debug((DEBUG_DEBUG, "SSL_write return fell through - errno %d returning retval %d", errno, retval));
      return retval; /* unknown error, assume block or success*/
    }
  }
  Debug((DEBUG_DEBUG, "SSL_write return fell through(2) - errno %d returning retval %d", errno, retval));
  return retval;
}

int ssl_send(struct Client *cptr, const char *buf, unsigned int len)
{
  char fmt[16];

  if (!cli_socket(cptr).ssl)
    return write(cli_fd(cptr), buf, len);

  /*
   * XXX HACK
   *
   * Incomplete SSL writes must be retried with the same write buffer;
   * at this point SSL_write usually fails, so the data must be queued.
   * We're abusing the normal send queue for this.
   * Also strip \r\n from message, as sendrawto_one adds it later
    this hack sucks. it conflicted with prority queues - caused random ssl disconnections for YEARS. In summery, this hack ==
   */
  ircd_snprintf(0, fmt, sizeof(fmt), "%%.%us", len - 2);
  sendrawto_one(cptr, fmt, buf);
  send_queued(cptr);
  return len;
}

int ssl_murder(void *ssl, int fd, const char *buf)
{
  if (!ssl) {
    write(fd, buf, strlen(buf));
  } else {
    SSL_write((SSL *) ssl, buf, strlen(buf));
    SSL_free((SSL *) ssl);
  }
  close(fd);
  return 0;
}

void ssl_free(struct Socket *socketh)
{
  if (!socketh->ssl)
    return;
  SSL_free(socketh->ssl);
}

char *ssl_get_cipher(SSL *ssl)
{
  static char buf[400];
  int bits;
  const SSL_CIPHER *c;

  buf[0] = '\0';
  strcpy(buf, SSL_get_version(ssl));
  strcat(buf, "-");
  strcat(buf, SSL_get_cipher(ssl));
  c = SSL_get_current_cipher(ssl);
  SSL_CIPHER_get_bits(c, &bits);
  strcat(buf, "-");
  strcat(buf, (char *)itoa(bits));
  strcat(buf, "bits");
  return (buf);
}

int ssl_connect(struct Socket* sock)
{
  int r = 0;

  if (!sock->ssl) {
    sock->ssl = SSL_new(ssl_client_ctx);
    SSL_set_fd(sock->ssl, sock->s_fd);
  }

  r = SSL_connect(sock->ssl);
  if (r<=0) {
    if ((SSL_get_error(sock->ssl, r) == SSL_ERROR_WANT_WRITE) || (SSL_get_error(sock->ssl, r) == SSL_ERROR_WANT_READ))
      return 0; /* Needs to call SSL_connect() again */
    else
      return -1; /* Fatal error */
  }
  return 1; /* Connection complete */
}

char* ssl_get_fingerprint(SSL *ssl)
{
  X509* cert;
  unsigned int n = 0;
  unsigned char md[EVP_MAX_MD_SIZE];
  const EVP_MD *digest = EVP_sha256();
  static char hex[BUFSIZE + 1];

  cert = SSL_get_peer_certificate(ssl);

  if (!(cert))
    return NULL;

  if (!X509_digest(cert, digest, md, &n))
  {
    X509_free(cert);
    return NULL;
  }

  binary_to_hex(md, hex, n);
  X509_free(cert);

  return (hex);
}

void sslfail(char *txt)
{
  unsigned long err = ERR_get_error();
  char string[120];

  if (!err) {
    Debug((DEBUG_DEBUG, "%s: poof", txt));
  } else {
    ERR_error_string(err, string);
    Debug((DEBUG_FATAL, "%s: %s", txt, string));
  }
}

void binary_to_hex(unsigned char *bin, char *hex, int length)
{
  static const char trans[] = "0123456789ABCDEF";
  int i;

  for(i = 0; i < length; i++)
  {
    hex[i  << 1]      = trans[bin[i] >> 4];
    hex[(i << 1) + 1] = trans[bin[i] & 0xf];
  }

  hex[i << 1] = '\0';
}

#endif /* USE_SSL */
