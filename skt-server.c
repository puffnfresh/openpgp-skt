#include <assert.h>
#include <stdarg.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <qrencode.h>
#include <gnutls/gnutls.h>
#include <gnutls/crypto.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdbool.h>
#include <unistd.h>
#include <gpgme.h>
#include <uv.h>

const char * psk_id_hint = "openpgp-skt";
const char schema[] = "OPENPGP+SKT";
const char priority[] = "NORMAL:-CTYPE-ALL"
  ":%SERVER_PRECEDENCE:%NO_TICKETS"
  ":-VERS-TLS1.0:-VERS-TLS1.1:-VERS-DTLS1.0:-VERS-DTLS1.2"
  ":-CURVE-SECP224R1:-CURVE-SECP192R1"
  ":-SIGN-ALL"
  ":-KX-ALL:+ECDHE-PSK:+DHE-PSK"
  ":-3DES-CBC:-CAMELLIA-128-CBC:-CAMELLIA-256-CBC";
const char pgp_begin[] = "-----BEGIN PGP PRIVATE KEY BLOCK-----";
const char pgp_end[] = "\n-----END PGP PRIVATE KEY BLOCK-----";


#define PSK_BYTES 16

struct session_status;

int print_qrcode(FILE* f, const QRcode* qrcode);
int print_address_name(struct sockaddr_storage *addr, char *paddr, size_t paddrsz, int *port);
void its_all_over(struct session_status *status, const char *fmt, ...);
void session_status_connect(uv_stream_t* server, int status);
int session_status_gather_secret_keys(struct session_status *status);
void session_status_release_all_gpg_keys(struct session_status *status);
void session_status_display_key_menu(struct session_status *status, FILE *f);
int session_status_close_tls(struct session_status *status);


struct session_status {
  uv_loop_t *loop;
  uv_tcp_t listen_socket;
  uv_tcp_t accepted_socket;
  gnutls_datum_t psk;
  char addrp[INET6_ADDRSTRLEN];
  int port;
  char caddrp[INET6_ADDRSTRLEN];
  int cport;
  char pskhex[PSK_BYTES*2 + 1];
  struct sockaddr_storage sa_serv_storage;
  struct sockaddr_storage sa_cli_storage;
  int sa_serv_storage_sz;
  int sa_cli_storage_sz;
  gnutls_session_t session;
  gpgme_ctx_t gpgctx;
  gpgme_ctx_t incoming;
  char *incomingdir;
  gnutls_datum_t incomingkey;
  size_t incomingkeylen;
  gpgme_key_t *keys;
  size_t num_keys;
  size_t keylist_offset;
  struct ifaddrs *ifap;
  char tlsreadbuf[65536];
  size_t start;
  size_t end;
  bool handshake_done;
  bool active;
  uv_tty_t input;
  int log_level;
};


void session_status_free(struct session_status *status) {
  if (status) {
    session_status_release_all_gpg_keys(status);
    if (status->gpgctx)
      gpgme_release(status->gpgctx);
    if (status->incoming)
      gpgme_release(status->incoming);
    if (status->incomingkey.data) {
      free(status->incomingkey.data);
    }
    if (status->incomingdir) {
      /* FIXME: really tear down the ephemeral homedir */
      if (rmdir(status->incomingdir))
        fprintf(stderr, "failed to rmdir('%s'): (%d) %s\n", status->incomingdir, errno, strerror(errno));

      /* FIXME: should we also try to kill all running daemons?*/
      free(status->incomingdir);
    }
    if (status->session) {
      session_status_close_tls(status);
    }
    if (status->ifap)
      freeifaddrs(status->ifap);
    free(status);
  }
}


void input_alloc_cb(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
  /*  struct session_status *status = handle->data; */
  buf->base = malloc(1);
  buf->len = buf->base ? 1 : 0;
}

ssize_t session_status_gpgme_write(void *h, const void *buf, size_t sz) {
  struct session_status *status = h;
  if (status->log_level > 3)
    fprintf(stderr, "got %zd octets of data from gpgme (%p)\n", sz, (void*)status);
  int rc = gnutls_record_send(status->session, buf, sz); /* FIXME: blocking */
  if (rc < 0) {
    switch (rc) {
    case GNUTLS_E_AGAIN:
      errno = EAGAIN;
      return -1;
    case GNUTLS_E_INTERRUPTED:
      errno = EINTR;
      return -1;
    default:
      fprintf(stderr, "gnutls_record_send() failed: (%d) %s\n", rc, gnutls_strerror(rc));
      /* FIXME: is this a reasonable value for errno when we don't know the error? */
      errno = EINVAL;
      return -1;
    }
  }
  return sz;
}

/* FIXME: should be const, but gpgme is cranky */
struct gpgme_data_cbs gpg_callbacks = { .write = session_status_gpgme_write };

int session_status_send_key(struct session_status *status, gpgme_key_t key) {
  int rc = 0;
  gpgme_error_t gerr = 0;
  gpgme_export_mode_t mode = GPGME_EXPORT_MODE_MINIMAL | GPGME_EXPORT_MODE_SECRET;
  char *pattern = NULL;
  gpgme_data_t data = NULL;

  status->active = true;
  gpgme_set_armor(status->gpgctx, 1);
  rc = asprintf(&pattern, "0x%s", key->fpr);
  if (rc == -1) {
    fprintf(stderr, "failed to malloc appropriately!\n");
    return -1;
  }
  if ((gerr = gpgme_data_new_from_cbs(&data, &gpg_callbacks, status))) {
    free(pattern);
    fprintf(stderr, "failed to make new gpgme_data_t object: (%d) %s\n", gerr, gpgme_strerror(gerr));
    return -1;
  }
  /* FIXME: blocking! */
  if ((gerr = gpgme_op_export(status->gpgctx, pattern, mode, data))) {
    free(pattern);
    fprintf(stderr, "failed to export key: (%d) %s\n", gerr, gpgme_strerror(gerr));
    return -1;
  }
  free(pattern);

  gpgme_data_release(data);
  return 0;
}

void input_read_cb(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
  struct session_status *status = stream->data;
  if (nread > 0) {
    int c = buf->base[0];
    if (c == 3) {
      its_all_over(status, "got ctrl-c\n");
    } else if (c == 4) {
      its_all_over(status, "got ctrl-d\n");
    } else if (tolower(c) == 'q' || c == 0x1B /* ESC */) {
      its_all_over(status, "quitting\n");
    } else if (status->incomingdir) {
      if (status->log_level > 2)
        fprintf(stderr, "In passive mode.  Cannot send keys.  Quit and reconnect to take the active role.\n");
    } else if (c == '0') {
      fprintf(stderr, "FIXME: sending a file from active mode is not yet implemented!\n");
    } else if (c >= '1' && c <= '8') {
      int x = (c-'1') + status->keylist_offset;
      session_status_send_key(status, status->keys[x]);
    } else if (c == '9') {
      if (status->num_keys < 9)
        fprintf(stderr, "No more keys to display\n");
      status->keylist_offset += 8;
      if (status->keylist_offset >= status->num_keys)
        status->keylist_offset = 0;
      session_status_display_key_menu(status, stdout);
    } else {
      if (status->log_level > 2)
        fprintf(stderr, "Got %d (0x%02x) '%.1s'\n", buf->base[0], buf->base[0], isprint(buf->base[0])? buf->base : "_");
    }
  } else if (nread < 0) {
    its_all_over(status, "Got error during input_read_cb: (%d) %s\n", nread, uv_strerror(nread));
  }
  if (buf && buf->base)
    free(buf->base);
}

void input_close_cb(uv_handle_t *handle) {
  struct session_status *status = handle->data;
  int rc;
  if ((rc = uv_tty_set_mode(&status->input, UV_TTY_MODE_NORMAL)))
    fprintf(stderr, "failed to switch input back to normal mode: (%d) %s\n", rc, uv_strerror(rc));
}

int session_status_setup_incoming(struct session_status *status) {
  gpgme_error_t gerr;
  int rc;
  char *xdg = getenv("XDG_RUNTIME_DIR");
  bool xdgf = false;

  assert(status->incoming == NULL);
  
  if (xdg != NULL) {
    rc = asprintf(&xdg, "/run/user/%d", getuid());
    if (rc == -1) {
      fprintf(stderr, "failed to guess user ID during ephemeral GnuPG setup.\r\n");
      return -1;
    }
    xdgf = true;
  }

  if (F_OK != access(xdg, W_OK)) {
    fprintf(stderr, "We don't have write access to '%s' for GnuPG ephemeral dir, falling back...\n", xdg);
    free(xdg);
    xdgf = false;
    xdg = getenv("TMPDIR");
    if (xdg == NULL || (F_OK != access(xdg, W_OK))) {
      if (xdg != NULL)
        fprintf(stderr, "We don't have write access to $TMPDIR ('%s') for GnuPG ephemeral dir, falling back to /tmp\n", xdg);
      xdg = "/tmp";
    }
  }
       
  rc = asprintf(&status->incomingdir, "%s/skt-server.XXXXXX", xdg);
  if (rc == -1) {
    fprintf(stderr, "Failed to allocate ephemeral GnuPG directory name in %s\n", xdg);
    goto fail;
  }
  if (NULL == mkdtemp(status->incomingdir)) {
    fprintf(stderr, "failed to generate an ephemeral GnuPG homedir from template '%s'\n", status->incomingdir);
    free(status->incomingdir);
    status->incomingdir = NULL;
    goto fail;
  }
  
  if ((gerr = gpgme_new(&status->incoming))) {
    fprintf(stderr, "gpgme_new failed when setting up ephemeral incoming directory: (%d), %s\n",
            gerr, gpgme_strerror(gerr));
    goto fail;
  }
  if ((gerr = gpgme_ctx_set_engine_info(status->incoming, GPGME_PROTOCOL_OpenPGP, NULL, status->incomingdir))) {
    fprintf(stderr, "gpgme_ctx_set_engine_info failed for ephemeral homedir %s: (%d), %s\n", status->incomingdir, gerr, gpgme_strerror(gerr));
    goto fail;
  }

  return 0;
 fail:
  if (xdgf)
    free(xdg);
  if (status->incomingdir) {
    if (rmdir(status->incomingdir))
      fprintf(stderr, "failed to rmdir('%s'): (%d) %s\n", status->incomingdir, errno, strerror(errno));
    free(status->incomingdir);
    status->incomingdir = NULL;
  }
  return -1;
}

struct session_status * session_status_new(uv_loop_t *loop, int log_level) {
  struct session_status *status = calloc(1, sizeof(struct session_status));
  size_t pskhexsz = sizeof(status->pskhex);
  gpgme_error_t gerr = GPG_ERR_NO_ERROR;
  int rc;
  
  if (status) {
    status->loop = loop;
    status->log_level = log_level;
    status->sa_serv_storage_sz = sizeof (status->sa_serv_storage);
    status->sa_cli_storage_sz = sizeof (status->sa_cli_storage);

    if ((gerr = gpgme_new(&status->gpgctx))) {
      fprintf(stderr, "gpgme_new failed: (%d), %s\n", gerr, gpgme_strerror(gerr));
      goto fail;
    }
    if ((gerr = gpgme_ctx_set_engine_info(status->gpgctx, GPGME_PROTOCOL_OpenPGP, NULL, NULL))) {
      fprintf(stderr, "gpgme_ctx_set_engine_info failed: (%d), %s\n", gerr, gpgme_strerror(gerr));
      goto fail;
    }

    /* choose random number */  
    if ((rc = gnutls_key_generate(&status->psk, PSK_BYTES))) {
      fprintf(stderr, "failed to get randomness: (%d) %s\n", rc, gnutls_strerror(rc));
      goto fail;
    }
    if ((rc = gnutls_hex_encode(&status->psk, status->pskhex, &pskhexsz))) {
      fprintf(stderr, "failed to encode PSK as a hex string: (%d) %s\n", rc, gnutls_strerror(rc));
      goto fail;
    }
    if (pskhexsz != sizeof(status->pskhex)) {
      fprintf(stderr, "bad calculation for psk size\n");
      goto fail;
    }
    for (int ix = 0; ix < sizeof(status->pskhex)-1; ix++)
      status->pskhex[ix] = toupper(status->pskhex[ix]);
  }
 return status;
 fail:
  session_status_free(status);
  return NULL;
}

int session_status_choose_address(struct session_status* status) {
  struct ifaddrs *ifa;
  struct sockaddr *myaddr = NULL;
  int myfamily = 0;
  int rc;
  /*  int optval = 1; */
  
  /* pick an IP address with getifaddrs instead of using in6addr_any */
  if (getifaddrs(&status->ifap)) {
    fprintf(stderr, "getifaddrs failed: (%d) %s\n", errno, strerror(errno));
    return -1;
  }
  for (ifa = status->ifap; ifa; ifa = ifa->ifa_next) {
    char addrstring[INET6_ADDRSTRLEN];
    bool skip = false;
    int family = 0;
    
    if (ifa->ifa_addr) {
      family = ((struct sockaddr_storage*)(ifa->ifa_addr))->ss_family;
      void * ptr = NULL;
      if (family == AF_INET6)
        ptr = &((struct sockaddr_in6*)(ifa->ifa_addr))->sin6_addr;
      else if (family == AF_INET)
        ptr = &((struct sockaddr_in*)(ifa->ifa_addr))->sin_addr;
      else if (family == AF_PACKET) 
        skip = true; /* struct rtnl_link_stats *stats = ifa->ifa_data */
      if (!skip)
        inet_ntop(family, ptr, addrstring, sizeof(addrstring));
      else
        strcpy(addrstring, "<unknown family>");
    } else {
      strcpy(addrstring, "<no address>");
    }
    if (ifa->ifa_flags & IFF_LOOPBACK) {
      if (status->log_level > 2)
        fprintf(stderr, "skipping %s because it is loopback\n", ifa->ifa_name);
      continue;
    }
    if (!(ifa->ifa_flags & IFF_UP)) {
      if (status->log_level > 2)
        fprintf(stderr, "skipping %s because it is not up\n", ifa->ifa_name);
      continue;
    }
    if (!skip) {
      if (status->log_level > 2)
        fprintf(stdout, "%s %s: %s (flags: 0x%x)\n", myaddr==NULL?"*":" ", ifa->ifa_name, addrstring, ifa->ifa_flags);
      /* FIXME: we're just taking the first up, non-loopback address */
      /* be cleverer about prefering wifi, preferring link-local addresses, and RFC1918 addressses. */
      if (myaddr == NULL) {
        myfamily = family;
        myaddr = ifa->ifa_addr;
      }
    }
  }

  if (myfamily == 0) {
    fprintf(stderr, "could not find an acceptable address to bind to.\n");
    return -1;
  }
  
  /* open listening socket */
  if ((rc = uv_tcp_init(status->loop, &status->listen_socket))) {
    fprintf(stderr, "failed to allocate a socket: (%d) %s\n", rc, uv_strerror(rc));
    return -1;
  }
  /* FIXME: i don't know how to set SO_REUSEADDR for libuv.  maybe we don't need it, though.
  if ((rc = setsockopt(status->listen_socket, SOL_SOCKET, SO_REUSEADDR, (void *) &optval, sizeof(int)))) {
    fprintf(stderr, "failed to set SO_REUSEADDR: (%d) %s\n", errno, strerror(errno));
    return -1;
  }
  */
  if ((rc = uv_tcp_bind(&status->listen_socket, myaddr, 0))) {
    fprintf(stderr, "failed to bind: (%d) %s\n", rc, uv_strerror(rc));
    return -1;
  }    
  if ((rc = uv_tcp_getsockname(&status->listen_socket, (struct sockaddr *) &status->sa_serv_storage, &status->sa_serv_storage_sz))) {
    fprintf(stderr, "failed to uv_tcp_getsockname: (%d) %s\n", rc, uv_strerror(rc));
    return -1;
  }
  if (status->sa_serv_storage_sz > sizeof(status->sa_serv_storage)) {
    fprintf(stderr, "needed more space (%d) than expected (%zd) for getsockname\n", status->sa_serv_storage_sz, sizeof(status->sa_serv_storage));
    return -1;
  }
  if (status->sa_serv_storage.ss_family != myfamily) {
    fprintf(stderr, "was expecting address family %d after binding, got %d\n", myfamily, status->sa_serv_storage.ss_family);
    return -1;
  }
  if (print_address_name(&status->sa_serv_storage, status->addrp, sizeof(status->addrp), &status->port))
    return -1;
  status->listen_socket.data = status;
  if ((rc = uv_listen((uv_stream_t*)(&status->listen_socket), 0, session_status_connect))) {
    fprintf(stderr, "failed to listen: (%d) %s\n", errno, strerror(errno));
    return -1;
  }
  return 0;
}


int get_psk_creds(gnutls_session_t session, const char* username, gnutls_datum_t* key) {
  struct session_status *status;
  status = gnutls_session_get_ptr(session);
  
  if (status->log_level > 2)
    fprintf(stderr, "sent username: %s, PSK: %s\n",
            username, /* dangerous: random bytes from the network! */
            status->pskhex); 
  key->size = status->psk.size;
  key->data = gnutls_malloc(status->psk.size);
  if (!key->data)
    return GNUTLS_E_MEMORY_ERROR;
  memcpy(key->data, status->psk.data, status->psk.size);
  return GNUTLS_E_SUCCESS;
}

void skt_log(int level, const char* data) {
  fprintf(stderr, "S:|<%d>| %s%s", level, data, data[strlen(data)-1] == '\n' ? "" : "\n");
}


int print_qrcode(FILE* f, const QRcode* qrcode) {
  const struct { char *data; size_t size; }  out[] = {
    { .data = "\xe2\x96\x88", .size = 3 }, /* U+2588 FULL BLOCK */
    { .data = "\xe2\x96\x80", .size = 3 }, /* U+2580 UPPER HALF BLOCK */
    { .data = "\xe2\x96\x84", .size = 3 }, /* U+2584 LOWER HALF BLOCK */
    { .data = " ", .size = 1 }, /* U+0020 SPACE */
  };
  const int margin = 2;
  int mx, my;

  if (1 != fwrite("\n", 1, 1, f)) {
    fprintf(stderr, "failed to write start of qrcode\n");
    return -1;
  }
  for (my = 0; my < margin; my++) {
    for (mx = 0; mx < qrcode->width + margin*4; mx++)
      if (1 != fwrite(out[0].data, out[0].size, 1, f)) {
        fprintf(stderr, "failed at upper margin of qrcode\n");
        return -1;
      }
    if (1 != fwrite("\n", 1, 1, f)) {
      fprintf(stderr, "failed writing newline into QR code in upper margin\n");
      return -1;
    }
  }
  
  for (int iy = 0; iy < qrcode->width; iy+= 2) {
    for (mx = 0; mx < margin*2; mx++)
      if (1 != fwrite(out[0].data, out[0].size, 1, f)) {
        fprintf(stderr, "failed at left margin of qrcode in row %d\n", iy);
        return -1;
      }
    for (int ix = 0; ix < qrcode->width; ix++) {
      int n = (qrcode->data[iy*qrcode->width + ix] & 0x01) << 1;
      if (iy+1 < qrcode->width)
        n += (qrcode->data[(iy+1)*qrcode->width + ix] & 0x01);
      if (1 != fwrite(out[n].data, out[n].size, 1, f)) {
        fprintf(stderr, "failed writing QR code at (%d,%d)\n", ix, iy);
        return -1;
      }
    }
    for (mx = 0; mx < margin*2; mx++)
      if (1 != fwrite(out[0].data, out[0].size, 1, f)) {
        fprintf(stderr, "failed at right margin of qrcode in row %d\n", iy);
        return -1;
      }
    if (1 != fwrite("\n", 1, 1, f)) {
      fprintf(stderr, "failed writing newline into QR code after line %d\n", iy);
      return -1;
    }
  }
  
  for (my = 0; my < margin; my++) {
    for (mx = 0; mx < qrcode->width + margin*4; mx++)
      if (1 != fwrite(out[0].data, out[0].size, 1, f)) {
        fprintf(stderr, "failed at lower margin of qrcode\n");
        return -1;
      }
    if (1 != fwrite("\n", 1, 1, f)) {
      fprintf(stderr, "failed writing newline into QR code in lower margin\n");
      return -1;
    }
  }

  if (fflush(f))
    fprintf(stderr, "Warning: failed to flush QR code stream: (%d) %s\n", errno, strerror(errno));

  return 0;
}

int print_address_name(struct sockaddr_storage *addr, char *paddr, size_t paddrsz, int *port) {
  if (addr->ss_family == AF_INET6) {
    struct sockaddr_in6 *sa_serv_full;
    sa_serv_full = (struct sockaddr_in6 *)addr;
    if (NULL == inet_ntop(addr->ss_family, &(sa_serv_full->sin6_addr), paddr, paddrsz)) {
      fprintf(stderr, "inet_ntop failed (%d) %s\n", errno, strerror(errno));
      return -1;
    }
    *port = ntohs(sa_serv_full->sin6_port);
  } else if (addr->ss_family == AF_INET) {
    struct sockaddr_in *sa_serv_full;
    sa_serv_full = (struct sockaddr_in *)addr;
    if (NULL == inet_ntop(addr->ss_family, &(sa_serv_full->sin_addr), paddr, paddrsz)) {
      fprintf(stderr, "inet_ntop failed (%d) %s\n", errno, strerror(errno));
      return -1;
    }
    *port = ntohs(sa_serv_full->sin_port);
  } else {
    fprintf(stderr, "unrecognized address family %d\n", addr->ss_family);
    return -1;
  }
  return 0;
}

int session_status_close_tls(struct session_status *status) {
  int rc;
  assert(status->session);
  if ((rc = gnutls_bye(status->session, GNUTLS_SHUT_RDWR))) {
    fprintf(stderr, "gnutls_bye got error (%d) %s\n", rc, gnutls_strerror(rc));
    return -1;
  }
  gnutls_deinit(status->session);
  status->session = NULL;
  if ((rc = uv_read_stop((uv_stream_t*)&status->accepted_socket))) {
    fprintf(stderr, "failed to stop reading the TLS stream (%d) %s\n", rc, uv_strerror(rc));
    return -1;
  }    
  return 0;
}

void its_all_over(struct session_status *status, const char *fmt, ...) {
  va_list ap;
  int rc;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  /* FIXME: how to tear it all down? */
  if (status->session) {
    session_status_close_tls(status);
  }
  if (status->input.data && !uv_is_closing((uv_handle_t*)(&status->input))) {
    if ((rc = uv_tty_reset_mode()))
      fprintf(stderr, "failed to uv_tty_reset_mode: (%d) %s\n", rc, uv_strerror(rc));
    uv_close((uv_handle_t*)(&status->input), input_close_cb);
  }
  uv_stop(status->loop);
}

/* FIXME: this would only be used in the event of a fully asynchronous
   write.  however, i don't see how to keep track of the memory being
   written correctly in that case.

void session_status_write_done(uv_write_t* req, int x) {
  if (x) {
    fprintf(stderr, "write failed: (%d) %s\n", x, uv_strerror(x));
    return;
  }
  struct session_status *status = req->handle->data;
  
  free(req);
}
*/

ssize_t session_status_gnutls_push_func(gnutls_transport_ptr_t ptr, const void* buf, size_t sz) {
  struct session_status *status = ptr;
  int rc;
  /* FIXME: be more asynchronous in writes; here we're just trying to be synchronous */
  
  /* FIXME: i do not like casting away constness here */
  uv_buf_t b[] = {{ .base = (void*) buf, .len = sz }}; 

  rc = uv_try_write((uv_stream_t*)(&status->accepted_socket), b, sizeof(b)/sizeof(b[0]));
  if (rc >= 0)
    return rc;
  fprintf(stderr, "got error %d (%s) when trying to write %zd octets\n", rc, uv_strerror(rc), sz);
  if (rc == UV_EAGAIN) {
    gnutls_transport_set_errno(status->session, EAGAIN);
    return -1;
  }
  gnutls_transport_set_errno(status->session, EIO);
  return -1;
}

ssize_t session_status_gnutls_pull_func(gnutls_transport_ptr_t ptr, void* buf, size_t sz) {
  struct session_status *status = ptr;
  int available = status->end - status->start;
  if (uv_is_closing((uv_handle_t*)(&status->accepted_socket)))
    return 0;
  if (status->end == status->start) {
    gnutls_transport_set_errno(status->session, EAGAIN);
    return -1;
  }
  /* FIXME: this seems like an extra unnecessary copy.  can we arrange
     it so that the buffer gets passed through to the uv_alloc_cb? */
  if (sz >= status->end - status->start) {
    memcpy(buf, status->tlsreadbuf + status->start, available);
    status->start = status->end = 0;
    return available;
  } else {
    memcpy(buf, status->tlsreadbuf + status->start, sz);
    status->start += sz;
    return sz;
  }
}

void session_status_alloc_cb(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
  struct session_status *status = handle->data;
  assert(handle == (uv_handle_t*)(&status->accepted_socket));
  buf->base = status->tlsreadbuf + status->end;
  buf->len = sizeof(status->tlsreadbuf) - status->end;
  /* FIXME: should consider how to read partial buffers, if these don't match a TLS record */
}

void session_status_handshake_done(struct session_status *status) {
  char *desc;
  int rc;
  desc = gnutls_session_get_desc(status->session);
  fprintf(stdout, "TLS handshake complete: %s\n", desc);
  gnutls_free(desc);
  status->handshake_done = true;
  /* FIXME: should flush all input before starting to respond to it */
  if ((rc = uv_tty_init(status->loop, &status->input, 0, 1))) {
    fprintf(stderr, "failed to grab stdin for reading, using passive mode only: (%d) %s\n", rc, uv_strerror(rc));
  } else if ((rc = uv_tty_set_mode(&status->input, UV_TTY_MODE_RAW))) {
    fprintf(stderr, "failed to switch input to raw mode, using passive mode only: (%d) %s\n", rc, uv_strerror(rc));
    uv_close((uv_handle_t*)(&status->input), input_close_cb);
  } else if ((rc = uv_read_start((uv_stream_t*)(&status->input), input_alloc_cb, input_read_cb))) {
    fprintf(stderr, "failed to start reading from stdin, using passive mode only: (%d) %s\n", rc, uv_strerror(rc));
    uv_close((uv_handle_t*)(&status->input), input_close_cb);
  } else {
    status->input.data = status;
  }
  session_status_display_key_menu(status, stdout);
}

int session_status_gather_secret_keys(struct session_status *status) {
  gpgme_error_t gerr;
  int secret_only = 1;
  const char *pattern = NULL;
  fprintf(stdout, "Gathering a list of available OpenPGP secret keys...\n");
  if ((gerr = gpgme_op_keylist_start(status->gpgctx, pattern, secret_only))) {
    fprintf(stderr, "Failed to start gathering keys: (%d) %s\n", gerr, gpgme_strerror(gerr));
    return 1;
  }
  while (!gerr) {
    gpgme_key_t key = NULL;
    gerr = gpgme_op_keylist_next(status->gpgctx, &key);
    if (!gerr) {
      status->num_keys++;
      gpgme_key_t * update = realloc(status->keys, sizeof(status->keys[0]) * status->num_keys);
      if (update) {
        status->keys = update;
        status->keys[status->num_keys-1] = key;
      } else {
        fprintf(stderr, "out of memory allocating new gpgme_key_t\n");
        goto fail;
      }
    } else if (gpgme_err_code(gerr) != GPG_ERR_EOF) {
      fprintf(stderr, "Failed to get keys: (%d) %s\n", gerr, gpgme_strerror(gerr));
      goto fail;
    }
  }
  return 0;
 fail:
  if ((gerr = gpgme_op_keylist_end(status->gpgctx)))
    fprintf(stderr, "failed to gpgme_op_keylist_end(): (%d) %s\n", gerr, gpgme_strerror(gerr));
  session_status_release_all_gpg_keys(status);
  return 1;
}

void session_status_display_key_menu(struct session_status *status, FILE *f) {
  int numleft = status->num_keys - status->keylist_offset;
  if (numleft > 8)
    numleft = 8;

  fprintf(f, "To receive a key, ask the other device to send it.\n");
  fprintf(f, "To send a key, press its number:\n\n");
  for (int ix = 0; ix < numleft; ix++) {
    fprintf(f, "[%d] %s\n    %s\n", (int)(1 + ix),
            status->keys[status->keylist_offset + ix]->uids->uid,
            status->keys[status->keylist_offset + ix]->fpr);
  }
  if (status->num_keys > 8)
    fprintf(f, "\n[9] …more available keys (%zd total)…\n", status->num_keys);
  fprintf(f, "[0] <choose a file to send>\n");
}

void session_status_release_all_gpg_keys(struct session_status *status) {
  if (status->keys) {
    for (int ix = 0; ix < status->num_keys; ix++) {
      gpgme_key_release(status->keys[ix]);
    }
    free(status->keys);
    status->keys = NULL;
  }
}

/* appends SUFFIX to position POS in BASE, growing BASE if necessary */
int append_data(gnutls_datum_t *base, const gnutls_datum_t *suffix, size_t pos) {
  fprintf(stderr, "base->data: %p, suffix->size: %u, pos: %zu\n", base->data, suffix->size, pos);
  size_t newlen = pos + suffix->size;
  if (base->size < newlen) {
    unsigned char *newdata = realloc(base->data, newlen);
    if (!newdata)
      return ENOMEM;
    base->data = newdata;
    base->size = newlen;
  }
  memcpy(base->data + pos, suffix->data, suffix->size);
  return 0;
}

int session_status_ingest_key(struct session_status *status, unsigned char *ptr, size_t sz) {
  return ENOSYS;
}

/* look through the incoming data stream and if it contains a key, try
   to ingest it.  FIXME: it's a bit wasteful to perform this scan of
   the whole buffer every time a packet comes in; should really be
   done in true async form, with the state stored in status someplace. */
int session_status_try_incoming_keys(struct session_status *status) {
  unsigned char *key = status->incomingkey.data;
  size_t sz = status->incomingkeylen;
  size_t consumed = 0;
  unsigned char *end;
  int rc;
  int ret = 0;

  while (1) {
    if (sz < sizeof(pgp_begin))
      break; /* just not big enough yet */
    if (memcmp(pgp_begin, key, sizeof(pgp_begin)-1))
      return EINVAL; /* it's gotta start with the usual header */
    if (!(key[sizeof(pgp_begin)-1] == '\r' || key[sizeof(pgp_begin)-1] == '\n'))
      return EINVAL; /* needs a trailing newline, however that's formed */
    
    /* FIXME: ensure that we just get comments and headers between the
       begin and end lines */
    
    end = memmem(key, sz, pgp_end, sizeof(pgp_end)-1);
    if (end == NULL)
      break; /* haven't reached the end yet */
    size_t pos = end + (sizeof(pgp_end)-1) - key;
    assert(pos <= sz);
    if (pos == sz)
      return 0; /* got everything but the final newline */
    if (key[pos] == '\n') {
      pos += 1;
    } else if (key[pos] == '\r') {
      if (pos+1 == sz)
        return 0; /* got everything but the final newline in a CRLF format */
      if (key[pos+1] == '\n')
        pos += 2;
      else
        return EINVAL;
    } else
      return EINVAL;
    /* at this point, POS points to the end of what we suspect to be an
       OpenPGP transferable private key. */
    if ((rc = session_status_ingest_key(status, key, pos))) {
      ret = rc;
    }
    consumed += pos;
    key += pos;
    sz -= pos;
  }
  if (consumed) {
    size_t leftovers = status->incomingkeylen - consumed;
    if (leftovers)
      memmove(status->incomingkey.data, status->incomingkey.data + consumed, leftovers);
    status->incomingkeylen = leftovers;
  }
  return ret;
}

int session_status_ingest_packet(struct session_status *status, gnutls_packet_t packet) {
  gnutls_datum_t data;
  int rc;

  assert(packet);
  gnutls_packet_get(packet, &data, NULL);
  if ((rc = append_data(&status->incomingkey, &data, status->incomingkeylen))) {
    fprintf(stderr, "Failed to append data: (%d) %s\n", rc, strerror(rc));
    return rc;
  }
  status->incomingkeylen += data.size;
  return session_status_try_incoming_keys(status);
}

void session_status_read_cb(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
  struct session_status *status = stream->data;
  int rc;
  assert(stream == (uv_stream_t*)(&status->accepted_socket));
  assert(buf->base == status->tlsreadbuf + status->end);
  status->end += nread;

  if (!status->handshake_done) {
    gnutls_alert_description_t alert;
    rc = gnutls_handshake(status->session);
    switch(rc) {
    case GNUTLS_E_WARNING_ALERT_RECEIVED:
      alert = gnutls_alert_get(status->session);
      fprintf(stderr, "Got GnuTLS alert (%d) %s\n", alert, gnutls_alert_get_name(alert));
      break;
    case GNUTLS_E_INTERRUPTED:
    case GNUTLS_E_AGAIN:
      if (status->log_level > 3)
        fprintf(stderr, "gnutls_handshake() got (%d) %s\n", rc, gnutls_strerror(rc));
      break;
    case GNUTLS_E_SUCCESS:
      session_status_handshake_done(status);
      break;
    default:
      its_all_over(status, "gnutls_handshake() got (%d) %s, fatal\n", rc, gnutls_strerror(rc));
    }
  } else while (1) {
    gnutls_packet_t packet = NULL;
    assert(status->session);
    rc = gnutls_record_recv_packet(status->session, &packet);
    if (rc == GNUTLS_E_AGAIN)
      return;
    if (rc == 0) {
      /* This is EOF from the remote peer.  We'd like to handle a
         half-closed stream if we're the active peer */
      assert(packet == NULL);
      if (status->active) {
        if (status->log_level > 0) 
          fprintf(stderr, "passive peer closed its side of the connection.\n");
      } else {
        if (status->incomingdir) {
          /* Now we've loaded as many of the keys as we will get.  We
             should now be in a mode where we ask the user to import
             them.  So we just need to close the TLS session and carry
             on. */
          if (!session_status_close_tls(status)) {
            fprintf(stderr, "Failed to close the TLS session!\n");
            return;
          }
        } else {
          its_all_over(status, "TLS session closed with nothing transmitted from either side!\n");
          return;
        }
      }
    } else if (rc < 0) {
      if (packet)
        gnutls_packet_deinit(packet);
      if (rc == GNUTLS_E_INTERRUPTED) {
        fprintf(stderr, "gnutls_record_recv_packet returned (%d) %s\n", rc, gnutls_strerror(rc));
      } else {
        /* this is an error */
        its_all_over(status, "Got an error in gnutls_record_recv_packet: (%d) %s\n", rc, gnutls_strerror(rc));
        return;
      }
    } else {
      if (status->active) {
        gnutls_packet_deinit(packet);
        its_all_over(status, "We are the active sender, but the other side sent stuff\n");
        return;
      }

      /* we're now in passive mode.  */
      if (!status->incomingdir) {
        if (session_status_setup_incoming(status)) {
          its_all_over(status, "Cannot import keys if the input is not an OpenPGP key\n");
          return;
        }
      }

      if ((rc = session_status_ingest_packet(status, packet))) {
        gnutls_packet_deinit(packet);
        its_all_over(status, "failed to ingest the packet: (%d) %s\n", rc, strerror(rc));
        return;
      }
      gnutls_packet_deinit(packet);
    }
  }
}

void session_status_cleanup_listener(uv_handle_t* handle) {
  struct session_status *status = handle->data;
  assert(handle == (uv_handle_t*)(&status->listen_socket));
  /* FIXME: do we need to clean up anything? */
}


void session_status_connect(uv_stream_t* server, int x) {
  int rc;
  struct session_status *status = server->data;
  assert(server == (uv_stream_t*)(&status->listen_socket));
  if (x < 0) 
    its_all_over(status, "connect callback called with status %d\n", x);
  else if ((rc = uv_tcp_init(status->loop, &status->accepted_socket))) 
    its_all_over(status, "failed to init accepted_socket: (%d) %s\n", rc, uv_strerror(rc));
  else {
    status->accepted_socket.data = status;
    if ((rc = uv_accept(server, (uv_stream_t*)(&status->accepted_socket))))
      its_all_over(status, "failed to init accepted_socket: (%d) %s\n", rc, uv_strerror(rc));
    else if ((rc = uv_tcp_getpeername(&status->accepted_socket, (struct sockaddr*)(&status->sa_cli_storage),
                                      &status->sa_cli_storage_sz)))
      its_all_over(status, "failed to getpeername of connected host: (%d) %s\n", rc, uv_strerror(rc));
    else {
      uv_close((uv_handle_t*)(&status->listen_socket), session_status_cleanup_listener);
      
      if (print_address_name(&status->sa_cli_storage, status->caddrp, sizeof(status->caddrp), &status->cport))
        return;
      fprintf(stdout, "A connection was made from %s%s%s:%d!\n",
              status->sa_cli_storage.ss_family==AF_INET6?"[":"",
              status->caddrp,
              status->sa_cli_storage.ss_family==AF_INET6?"]":"",
              status->cport
              );

      if ((rc = uv_read_start((uv_stream_t*)(&status->accepted_socket), session_status_alloc_cb, session_status_read_cb)))
        its_all_over(status, "failed to uv_read_start: (%d) %s\n", rc, uv_strerror(rc));
    }
  }
}




int main(int argc, const char *argv[]) {
  struct session_status *status;
  uv_loop_t loop;
  int rc;
  gnutls_psk_server_credentials_t creds = NULL;
  gnutls_priority_t priority_cache;
  char urlbuf[INET6_ADDRSTRLEN + 25 + 32];
  int urllen;
  QRcode *qrcode = NULL;
  FILE * inkey = NULL;
  const char *ll;
  int log_level;

  ll = getenv("LOG_LEVEL");
  log_level = ll ? atoi(ll) : 0;
  gpgme_check_version (NULL);
  gnutls_global_set_log_level(log_level);
  gnutls_global_set_log_function(skt_log);
  if ((rc = uv_loop_init(&loop))) {
    fprintf(stderr, "failed to init uv_loop: (%d) %s\n", rc, uv_strerror(rc));
    return -1;
  }

  if (argc > 1) {
    if (!strcmp(argv[1], "-")) {
      inkey = stdin;
    } else {
      inkey = fopen(argv[1], "r");
      if (inkey == NULL)
        fprintf(stderr, "could not read key '%s', instead waiting to receive key: (%d) %s\n",
                argv[1], errno, strerror(errno));
    }
  }

  status = session_status_new(&loop, log_level);
  if (!status) {
    fprintf(stderr, "Failed to initialize status object\n");
    return -1;
  }
  if (session_status_choose_address(status))
    return -1;

  if (session_status_gather_secret_keys(status))
    return -1;
  
  /* open tls server connection */
  if ((rc = gnutls_init(&status->session, GNUTLS_SERVER | GNUTLS_NONBLOCK))) {
    fprintf(stderr, "failed to init session: (%d) %s\n", rc, gnutls_strerror(rc));
    return -1;
  }
  gnutls_session_set_ptr(status->session, status);
  gnutls_transport_set_pull_function(status->session, session_status_gnutls_pull_func);
  gnutls_transport_set_push_function(status->session, session_status_gnutls_push_func);
  gnutls_transport_set_ptr(status->session, status);
  if ((rc = gnutls_psk_allocate_server_credentials(&creds))) {
    fprintf(stderr, "failed to allocate PSK credentials: (%d) %s\n", rc, gnutls_strerror(rc));
    return -1;
  }
  if ((rc = gnutls_psk_set_server_credentials_hint(creds, psk_id_hint))) {
    fprintf(stderr, "failed to set server credentials hint to '%s', ignoring…\n", psk_id_hint);
  }
  if ((rc = gnutls_psk_set_server_known_dh_params(creds, GNUTLS_SEC_PARAM_HIGH))) {
    fprintf(stderr, "failed to set server credentials known DH params: (%d) %s\n", rc, gnutls_strerror(rc));
    return -1;
  }
  gnutls_psk_set_server_credentials_function(creds, get_psk_creds);
  if ((rc = gnutls_credentials_set(status->session, GNUTLS_CRD_PSK, creds))) {
    fprintf(stderr, "failed to assign PSK credentials to GnuTLS server: (%d) %s\n", rc, gnutls_strerror(rc));
    return -1;
  }
  if ((rc = gnutls_priority_init(&priority_cache, priority, NULL))) {
    fprintf(stderr, "failed to set up GnuTLS priority: (%d) %s\n", rc, gnutls_strerror(rc));
    return -1;
  }
  if ((rc = gnutls_priority_set(status->session, priority_cache))) {
    fprintf(stderr, "failed to assign gnutls priority: (%d) %s\n", rc, gnutls_strerror(rc));
    return -1;
  }

  /* construct string */
  urlbuf[sizeof(urlbuf)-1] = 0;
  urllen = snprintf(urlbuf, sizeof(urlbuf)-1, "%s://%s@%s%s%s:%d", schema, status->pskhex,
                    status->sa_serv_storage.ss_family==AF_INET6?"[":"",
                    status->addrp,
                    status->sa_serv_storage.ss_family==AF_INET6?"]":"", status->port);
  if (urllen >= (sizeof(urlbuf)-1)) {
    fprintf(stderr, "buffer was somehow truncated.\n");
    return -1;
  }
  if (urllen < 5) {
    fprintf(stderr, "printed URL was somehow way too small (%d).\n", urllen);
    return -1;
  }
  fprintf(stdout, "%s\n", urlbuf);
      
  /* generate qrcode (can't use QR_MODE_AN because of punctuation in URL) */
  qrcode = QRcode_encodeString(urlbuf, 0, QR_ECLEVEL_L, QR_MODE_8, 0);
  if (qrcode == NULL) {
    fprintf(stderr, "failed to encode string as QRcode: (%d) %s\n", errno, strerror(errno));
    return -1;
  }
  
  /* display qrcode */
  if ((rc = print_qrcode(stdout, qrcode))) {
    fprintf(stderr, "failed to print qr code\n");
    return -1;
  }

  /* for test purposes... */
  if (status->log_level > 0)
    fprintf(stdout, "gnutls-cli --debug %d --priority %s --port %d --pskusername %s --pskkey %s %s\n",
            status->log_level, priority, status->port, psk_id_hint, status->pskhex, status->addrp);

  if ((rc = uv_run(&loop, UV_RUN_DEFAULT))) {
    while ((rc = uv_run(&loop, UV_RUN_ONCE))) {
      fprintf(stderr, "UV_RUN_ONCE returned %d\n", rc);
    }
  }
  fprintf(stderr, "Done with the loop\n");

  if (inkey) {
    /* FIXME: send key */
    char data[65536];
    if (status->log_level > 3)
      fprintf(stderr, "trying to write %s to client\n", (stdin == inkey) ? "standard input" : argv[1]);

    /* read from inkey, send to gnutls */
    while (!feof(inkey)) {
      size_t r;
      r = fread(data, 1, sizeof(data), inkey); /* FIXME: blocking */
      if (ferror(inkey)) {
        fprintf(stderr, "Error reading from input\n");
        return -1;
      } else {
        if (status->log_level > 3)
          fprintf(stderr, "trying to write %zd octets to client\n", r);
        while (r) {
          rc = GNUTLS_E_AGAIN;
          while (rc == GNUTLS_E_AGAIN || rc == GNUTLS_E_INTERRUPTED) {
            rc = gnutls_record_send(status->session, data, r); /* FIXME: blocking */
            if (rc < 0) {
              if (rc != GNUTLS_E_AGAIN && rc != GNUTLS_E_INTERRUPTED) {
                fprintf(stderr, "gnutls_record_send() failed: (%d) %s\n", rc, gnutls_strerror(rc));
                return -1;
              }
            } else {
              r -= rc;
            }
          }
        }
      }
    }
  }

  /* cleanup */
  session_status_free(status);
  gnutls_priority_deinit(priority_cache);
  gnutls_psk_free_server_credentials(creds);
  QRcode_free(qrcode);
  return 0;
}
