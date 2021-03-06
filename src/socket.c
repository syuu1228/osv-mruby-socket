/*
** socket.c - Socket module
**
** See Copyright Notice in mruby.h
*/

#include "mruby.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

#include "mruby/array.h"
#include "mruby/class.h"
#include "mruby/data.h"
#include "mruby/string.h"
#include "mruby/variable.h"
#include "mruby/ext/io.h"
#include "error.h"

#define E_SOCKET_ERROR             (mrb_class_get(mrb, "SocketError"))


static mrb_value
mrb_addrinfo_getaddrinfo(mrb_state *mrb, mrb_value klass)
{
  struct addrinfo hints, *res0, *res;
  struct sockaddr_un sun;
  mrb_value ai, ary, family, lastai, nodename, protocol, s, sa, service, socktype;
  mrb_int flags;
  int arena_idx, error;
  const char *hostname = NULL, *servname = NULL;

  ary = mrb_ary_new(mrb);
  arena_idx = mrb_gc_arena_save(mrb);  /* ary must be on arena! */

  s = mrb_str_new(mrb, (void *)&sun, sizeof(sun));

  family = socktype = protocol = mrb_nil_value();
  flags = 0;
  mrb_get_args(mrb, "oo|oooi", &nodename, &service, &family, &socktype, &protocol, &flags);

  if (mrb_string_p(nodename)) {
    hostname = mrb_str_to_cstr(mrb, nodename);
  } else if (mrb_nil_p(nodename)) {
    hostname = NULL;
  } else {
    mrb_raise(mrb, E_TYPE_ERROR, "nodename must be String or nil");
  }

  if (mrb_string_p(service)) {
    servname = mrb_str_to_cstr(mrb, service);
  } else if (mrb_fixnum_p(service)) {
    servname = mrb_str_to_cstr(mrb, mrb_funcall(mrb, service, "to_s", 0));
  } else if (mrb_nil_p(service)) {
    servname = NULL;
  } else {
    mrb_raise(mrb, E_TYPE_ERROR, "service must be String, Fixnum, or nil");
  }

  memset(&hints, 0, sizeof(hints));
  hints.ai_flags = flags;

  if (mrb_fixnum_p(family)) {
    hints.ai_family = mrb_fixnum(family);
  }

  if (mrb_fixnum_p(socktype)) {
    hints.ai_socktype = mrb_fixnum(socktype);
  }

  lastai = mrb_cv_get(mrb, klass, mrb_intern_cstr(mrb, "_lastai"));
  if (mrb_voidp_p(lastai)) {
    freeaddrinfo(mrb_voidp(lastai));
    mrb_cv_set(mrb, klass, mrb_intern_cstr(mrb, "_lastai"), mrb_nil_value());
  }

  error = getaddrinfo(hostname, servname, &hints, &res0);
  if (error) {
    mrb_raisef(mrb, E_SOCKET_ERROR, "getaddrinfo");
  }
  mrb_cv_set(mrb, klass, mrb_intern_cstr(mrb, "_lastai"), mrb_voidp_value(mrb, res0));

  for (res = res0; res != NULL; res = res->ai_next) {
    sa = mrb_str_new(mrb, (void *)res->ai_addr, res->ai_addrlen);
    ai = mrb_funcall(mrb, klass, "new", 4, sa, mrb_fixnum_value(res->ai_family), mrb_fixnum_value(res->ai_socktype), mrb_fixnum_value(res->ai_protocol));
    mrb_ary_push(mrb, ary, ai);
    mrb_gc_arena_restore(mrb, arena_idx);
  }

  freeaddrinfo(res0);
  mrb_cv_set(mrb, klass, mrb_intern_cstr(mrb, "_lastai"), mrb_nil_value());

  return ary;
}

static mrb_value
mrb_addrinfo_getnameinfo(mrb_state *mrb, mrb_value self)
{
  mrb_int flags;
  mrb_value ary, host, sastr, serv;
  int error;

  flags = 0;
  mrb_get_args(mrb, "|i", &flags);
  host = mrb_str_buf_new(mrb, NI_MAXHOST);
  serv = mrb_str_buf_new(mrb, NI_MAXSERV);

  sastr = mrb_iv_get(mrb, self, mrb_intern(mrb, "@sockaddr"));
  if (!mrb_string_p(sastr)) {
    mrb_raise(mrb, E_SOCKET_ERROR, "invalid sockaddr");
  }
  error = getnameinfo((struct sockaddr *)RSTRING_PTR(sastr), (socklen_t)RSTRING_LEN(sastr), RSTRING_PTR(host), NI_MAXHOST, RSTRING_PTR(serv), NI_MAXSERV, flags);
  if (error != 0) {
    mrb_raisef(mrb, E_SOCKET_ERROR, "getnameinfo");
  }
  ary = mrb_ary_new_capa(mrb, 2);
  mrb_str_resize(mrb, host, strlen(RSTRING_PTR(host)));
  mrb_ary_push(mrb, ary, host);
  mrb_str_resize(mrb, serv, strlen(RSTRING_PTR(serv)));
  mrb_ary_push(mrb, ary, serv);
  return ary;
}

static mrb_value
mrb_addrinfo_unix_path(mrb_state *mrb, mrb_value self)
{
  mrb_value sastr;

  sastr = mrb_iv_get(mrb, self, mrb_intern(mrb, "@sockaddr"));
  if (((struct sockaddr *)RSTRING_PTR(sastr))->sa_family != AF_UNIX)
    mrb_raise(mrb, E_SOCKET_ERROR, "need AF_UNIX address");
  return mrb_str_new_cstr(mrb, ((struct sockaddr_un *)RSTRING_PTR(sastr))->sun_path);
}

static mrb_value
sa2addrlist(mrb_state *mrb, const struct sockaddr *sa, socklen_t salen)
{
  mrb_value ary, host;
  unsigned short port;
  const char *afstr;

  switch (sa->sa_family) {
  case AF_INET:
    afstr = "AF_INET";
    port = ((struct sockaddr_in *)sa)->sin_port;
    break;
  case AF_INET6:
    afstr = "AF_INET6";
    port = ((struct sockaddr_in6 *)sa)->sin6_port;
    break;
  default:
    mrb_raise(mrb, E_ARGUMENT_ERROR, "bad af");
    return mrb_nil_value();
  }
  port = ntohs(port);
  host = mrb_str_buf_new(mrb, NI_MAXHOST);
  if (getnameinfo(sa, salen, RSTRING_PTR(host), NI_MAXHOST, NULL, 0, NI_NUMERICHOST) == -1)
    mrb_sys_fail(mrb, "getnameinfo");
  mrb_str_resize(mrb, host, strlen(RSTRING_PTR(host)));
  ary = mrb_ary_new_capa(mrb, 4);
  mrb_ary_push(mrb, ary, mrb_str_new_cstr(mrb, afstr));
  mrb_ary_push(mrb, ary, mrb_fixnum_value(port));
  mrb_ary_push(mrb, ary, host);
  mrb_ary_push(mrb, ary, host);
  return ary;
}

static int
socket_fd(mrb_state *mrb, mrb_value sock)
{
  return mrb_fixnum(mrb_funcall(mrb, sock, "fileno", 0));
}

static int
socket_family(int s)
{
  struct sockaddr_storage ss;
  socklen_t salen;

  salen = sizeof(ss);
  if (getsockname(s, (struct sockaddr *)&ss, &salen) == -1)
    return AF_UNSPEC;
  return ss.ss_family;
}

static mrb_value
mrb_basicsocket_getpeereid(mrb_state *mrb, mrb_value self)
{ 
#ifdef HAVE_GETPEEREID
  mrb_value ary;
  gid_t egid;
  uid_t euid;
  int s;
  
  s = socket_fd(mrb, self);
  if (getpeereid(s, &euid, &egid) != 0)
    mrb_sys_fail(mrb, "getpeereid");

  ary = mrb_ary_new_capa(mrb, 2);
  mrb_ary_push(mrb, ary, mrb_fixnum_value((mrb_int)euid));
  mrb_ary_push(mrb, ary, mrb_fixnum_value((mrb_int)egid));
  return ary;
#else
  mrb_raise(mrb, E_RUNTIME_ERROR, "getpeereid is not avaialble on this system");
  return mrb_nil_value();
#endif
}

static mrb_value
mrb_basicsocket_getpeername(mrb_state *mrb, mrb_value self)
{ 
  struct sockaddr_storage ss;
  socklen_t salen;
  
  salen = sizeof(ss);
  if (getpeername(socket_fd(mrb, self), (struct sockaddr *)&ss, &salen) != 0)
    mrb_sys_fail(mrb, "getpeername");

  return mrb_str_new(mrb, (void *)&ss, salen);
}

static mrb_value
mrb_basicsocket_getsockname(mrb_state *mrb, mrb_value self)
{ 
  struct sockaddr_storage ss;
  socklen_t salen;
  
  salen = sizeof(ss);
  if (getsockname(socket_fd(mrb, self), (struct sockaddr *)&ss, &salen) != 0)
    mrb_sys_fail(mrb, "getsockname");

  return mrb_str_new(mrb, (void *)&ss, salen);
}

static mrb_value
mrb_basicsocket_getsockopt(mrb_state *mrb, mrb_value self)
{ 
  int opt, s;
  mrb_int family, level, optname;
  mrb_value c, data;
  socklen_t optlen;

  mrb_get_args(mrb, "ii", &level, &optname);
  s = socket_fd(mrb, self);
  optlen = sizeof(opt);
  if (getsockopt(s, level, optname, &opt, &optlen) == -1)
    mrb_sys_fail(mrb, "getsockopt");
  c = mrb_const_get(mrb, mrb_obj_value(mrb_class_get(mrb, "Socket")), mrb_intern(mrb, "Option"));
  family = socket_family(s);
  data = mrb_str_new(mrb, (char *)&opt, sizeof(int));
  return mrb_funcall(mrb, c, "new", 4, mrb_fixnum_value(family), mrb_fixnum_value(level), mrb_fixnum_value(optname), data);
}

static mrb_value
mrb_basicsocket_recv(mrb_state *mrb, mrb_value self)
{ 
  int n;
  mrb_int maxlen, flags = 0;
  mrb_value buf;

  mrb_get_args(mrb, "i|i", &maxlen, &flags);
  buf = mrb_str_buf_new(mrb, maxlen);
  n = recv(socket_fd(mrb, self), RSTRING_PTR(buf), maxlen, flags);
  if (n == -1)
    mrb_sys_fail(mrb, "recv");
  mrb_str_resize(mrb, buf, n);
  return buf;
}

static mrb_value
mrb_basicsocket_recvfrom(mrb_state *mrb, mrb_value self)
{ 
  int n;
  mrb_int maxlen, flags = 0;
  mrb_value ary, buf, sa;
  socklen_t socklen;

  mrb_get_args(mrb, "i|i", &maxlen, &flags);
  buf = mrb_str_buf_new(mrb, maxlen);
  socklen = sizeof(struct sockaddr_storage);
  sa = mrb_str_buf_new(mrb, socklen);
  n = recvfrom(socket_fd(mrb, self), RSTRING_PTR(buf), maxlen, flags, (struct sockaddr *)RSTRING_PTR(sa), &socklen);
  if (n == -1)
    mrb_sys_fail(mrb, "recvfrom");
  mrb_str_resize(mrb, buf, n);
  mrb_str_resize(mrb, sa, socklen);
  ary = mrb_ary_new_capa(mrb, 2);
  mrb_ary_push(mrb, ary, buf);
  mrb_ary_push(mrb, ary, sa);
  return ary;
}

static mrb_value
mrb_basicsocket_send(mrb_state *mrb, mrb_value self)
{ 
  int n;
  mrb_int flags;
  mrb_value dest, mesg;

  dest = mrb_nil_value();
  mrb_get_args(mrb, "Si|S", &mesg, &flags, &dest);
  if (mrb_nil_p(dest)) {
    n = send(socket_fd(mrb, self), RSTRING_PTR(mesg), RSTRING_LEN(mesg), flags);
  } else {
    n = sendto(socket_fd(mrb, self), RSTRING_PTR(mesg), RSTRING_LEN(mesg), flags, (const void *)RSTRING_PTR(dest), RSTRING_LEN(dest));
  }
  if (n == -1)
    mrb_sys_fail(mrb, "send");
  return mrb_fixnum_value(n);
}

static mrb_value
mrb_basicsocket_setnonblock(mrb_state *mrb, mrb_value self)
{ 
  int fd, flags;
  mrb_value bool;

  mrb_get_args(mrb, "o", &bool);
  fd = socket_fd(mrb, self);

  flags = fcntl(fd, F_GETFL, 0);
  if (flags == 1)
    mrb_sys_fail(mrb, "fcntl");
  if (mrb_test(bool))
    flags |= O_NONBLOCK;
  else
    flags &= ~O_NONBLOCK;
  if (fcntl(fd, F_SETFL, flags) == -1)
    mrb_sys_fail(mrb, "fcntl");
  return mrb_nil_value();
}

static mrb_value
mrb_basicsocket_setsockopt(mrb_state *mrb, mrb_value self)
{ 
  int argc, s;
  mrb_int level = 0, optname;
  mrb_value optval, so;

  argc = mrb_get_args(mrb, "o|io", &so, &optname, &optval);
  if (argc == 3) {
    if (!mrb_fixnum_p(so)) {
      mrb_raise(mrb, E_ARGUMENT_ERROR, "level is not an integer");
    }
    level = mrb_fixnum(so);
    if (mrb_string_p(optval)) {
      /* that's good */
    } else if (mrb_type(optval) == MRB_TT_TRUE || mrb_type(optval) == MRB_TT_FALSE) {
      mrb_int i = mrb_test(optval) ? 1 : 0;
      optval = mrb_str_new(mrb, (char *)&i, sizeof(i));
    } else if (mrb_fixnum_p(optval)) {
      mrb_int i = mrb_fixnum(optval);
      optval = mrb_str_new(mrb, (char *)&i, sizeof(i));
    } else {
      mrb_raise(mrb, E_ARGUMENT_ERROR, "optval should be true, false, an integer, or a string");
    }
  } else if (argc == 1) {
    if (strcmp(mrb_obj_classname(mrb, so), "Socket::Option") != 0)
      mrb_raisef(mrb, E_ARGUMENT_ERROR, "not an instance of Socket::Option");
    level = mrb_fixnum(mrb_funcall(mrb, so, "level", 0));
    optname = mrb_fixnum(mrb_funcall(mrb, so, "optname", 0));
    optval = mrb_funcall(mrb, so, "data", 0);
  } else {
    mrb_raisef(mrb, E_ARGUMENT_ERROR, "wrong number of arguments (%d for 3)", argc);
  }

  s = socket_fd(mrb, self);
  if (setsockopt(s, level, optname, RSTRING_PTR(optval), RSTRING_LEN(optval)) == -1)
    mrb_sys_fail(mrb, "setsockopt");
  return mrb_fixnum_value(0);
}

static mrb_value
mrb_basicsocket_shutdown(mrb_state *mrb, mrb_value self)
{ 
  mrb_int how = SHUT_RDWR;

  mrb_get_args(mrb, "|i", &how);
  if (shutdown(socket_fd(mrb, self), how) != 0)
    mrb_sys_fail(mrb, "shutdown");
  return mrb_fixnum_value(0);
}

static mrb_value
mrb_ipsocket_ntop(mrb_state *mrb, mrb_value klass)
{ 
  mrb_int af, n;
  char *addr, buf[50];

  mrb_get_args(mrb, "is", &af, &addr, &n);
  if ((af == AF_INET && n != 4) || (af == AF_INET6 && n != 16))
    mrb_raise(mrb, E_ARGUMENT_ERROR, "invalid address");
  if (inet_ntop(af, addr, buf, sizeof(buf)) == NULL)
    mrb_raise(mrb, E_ARGUMENT_ERROR, "invalid address");
  return mrb_str_new_cstr(mrb, buf);
}

static mrb_value
mrb_ipsocket_pton(mrb_state *mrb, mrb_value klass)
{ 
  mrb_int af, n;
  char *bp, buf[50];

  mrb_get_args(mrb, "is", &af, &bp, &n);
  if (n > sizeof(buf) - 1)
    mrb_raise(mrb, E_ARGUMENT_ERROR, "invalid address");
  memcpy(buf, bp, n);
  buf[n] = '\0';

  if (af == AF_INET) {
    struct in_addr in;
    if (inet_pton(AF_INET, buf, (void *)&in.s_addr) != 1)
      goto invalid;
    return mrb_str_new(mrb, (char *)&in.s_addr, 4);
  } else if (af == AF_INET6) {
    struct in6_addr in6;
    if (inet_pton(AF_INET6, buf, (void *)&in6.s6_addr) != 1)
      goto invalid;
    return mrb_str_new(mrb, (char *)&in6.s6_addr, 16);
  } else
    mrb_raise(mrb, E_ARGUMENT_ERROR, "unsupported address family");

invalid:
  mrb_raise(mrb, E_ARGUMENT_ERROR, "invalid address");
  return mrb_nil_value(); /* dummy */
}

static mrb_value
mrb_ipsocket_recvfrom(mrb_state *mrb, mrb_value self)
{ 
  struct sockaddr_storage ss;
  socklen_t socklen;
  mrb_value a, buf, pair;
  mrb_int flags, maxlen, n;
  int fd;

  fd = socket_fd(mrb, self);
  flags = 0;
  mrb_get_args(mrb, "i|i", &maxlen, &flags);
  buf = mrb_str_buf_new(mrb, maxlen);
  socklen = sizeof(ss);
  n = recvfrom(fd, RSTRING_PTR(buf), maxlen, flags,
  	       (struct sockaddr *)&ss, &socklen);
  if (n == -1) {
    mrb_sys_fail(mrb, "recvfrom");
  }
  mrb_str_resize(mrb, buf, n);
  a = sa2addrlist(mrb, (struct sockaddr *)&ss, socklen);
  pair = mrb_ary_new_capa(mrb, 2);
  mrb_ary_push(mrb, pair, buf);
  mrb_ary_push(mrb, pair, a);
  return pair;
}

static mrb_value
mrb_socket_gethostname(mrb_state *mrb, mrb_value cls)
{
  mrb_value buf;
  size_t bufsize;
  
#ifdef HOST_NAME_MAX
  bufsize = HOST_NAME_MAX + 1;
#else
  bufsize = 256;
#endif
  buf = mrb_str_buf_new(mrb, bufsize);
  if (gethostname(RSTRING_PTR(buf), bufsize) != 0)
    mrb_sys_fail(mrb, "gethostname");
  mrb_str_resize(mrb, buf, strlen(RSTRING_PTR(buf)));
  return buf;
}

static mrb_value
mrb_socket_accept(mrb_state *mrb, mrb_value klass)
{
  mrb_value ary, sastr;
  int s1;
  mrb_int s0;
  socklen_t socklen;

  mrb_get_args(mrb, "i", &s0);
  socklen = sizeof(struct sockaddr_storage);
  sastr = mrb_str_buf_new(mrb, socklen);
  s1 = accept(s0, (struct sockaddr *)RSTRING_PTR(sastr), &socklen);
  if (s1 == -1) {
    mrb_sys_fail(mrb, "accept");
  }
  // XXX: possible descriptor leakage here!
  mrb_str_resize(mrb, sastr, socklen);
  ary = mrb_ary_new_capa(mrb, 2);
  mrb_ary_push(mrb, ary, mrb_fixnum_value(s1));
  mrb_ary_push(mrb, ary, sastr);
  return ary;
}

static mrb_value
mrb_socket_bind(mrb_state *mrb, mrb_value klass)
{
  mrb_value sastr;
  int s;

  mrb_get_args(mrb, "iS", &s, &sastr);
  if (bind(s, (struct sockaddr *)RSTRING_PTR(sastr), (socklen_t)RSTRING_LEN(sastr)) == -1) {
    mrb_sys_fail(mrb, "bind");
  }
  return mrb_nil_value();
}

static mrb_value
mrb_socket_connect(mrb_state *mrb, mrb_value klass)
{
  mrb_value sastr;
  int s;

  mrb_get_args(mrb, "iS", &s, &sastr);
  if (connect(s, (struct sockaddr *)RSTRING_PTR(sastr), (socklen_t)RSTRING_LEN(sastr)) == -1) {
    mrb_sys_fail(mrb, "connect");
  }
  return mrb_nil_value();
}

static mrb_value
mrb_socket_listen(mrb_state *mrb, mrb_value klass)
{
  int backlog, s;

  mrb_get_args(mrb, "ii", &s, &backlog);
  if (listen(s, backlog) == -1) {
    mrb_sys_fail(mrb, "listen");
  }
  return mrb_nil_value();
}

static mrb_value
mrb_socket_sockaddr_family(mrb_state *mrb, mrb_value klass)
{
  mrb_value sa;

  mrb_get_args(mrb, "S", &sa);
  if (RSTRING_LEN(sa) < sizeof(struct sockaddr)) {
    mrb_raisef(mrb, E_SOCKET_ERROR, "invalid sockaddr (too short)");
  }
  return mrb_fixnum_value(((struct sockaddr *)RSTRING_PTR(sa))->sa_family);
}

static mrb_value
mrb_socket_sockaddr_un(mrb_state *mrb, mrb_value klass)
{
  struct sockaddr_un *sunp;
  mrb_value path, s;
  
  mrb_get_args(mrb, "S", &path);
  if (RSTRING_LEN(path) > sizeof(sunp->sun_path) - 1) {
    mrb_raisef(mrb, E_ARGUMENT_ERROR, "too long unix socket path (max: %ubytes)", (unsigned int)sizeof(sunp->sun_path) - 1);
  }
  s = mrb_str_buf_new(mrb, sizeof(struct sockaddr_un));
  sunp = (struct sockaddr_un *)RSTRING_PTR(s);
  sunp->sun_family = AF_UNIX;
  memcpy(sunp->sun_path, RSTRING_PTR(path), RSTRING_LEN(path));
  sunp->sun_path[RSTRING_LEN(path)] = '\0';
  mrb_str_resize(mrb, s, sizeof(struct sockaddr_un));
  return s;
}

static mrb_value
mrb_socket_socketpair(mrb_state *mrb, mrb_value klass)
{
  mrb_value ary;
  mrb_int domain, type, protocol;
  int sv[2];

  mrb_get_args(mrb, "iii", &domain, &type, &protocol);
  if (socketpair(domain, type, protocol, sv) == -1) {
    mrb_sys_fail(mrb, "socketpair");
  }
  // XXX: possible descriptor leakage here!
  ary = mrb_ary_new_capa(mrb, 2);
  mrb_ary_push(mrb, ary, mrb_fixnum_value(sv[0]));
  mrb_ary_push(mrb, ary, mrb_fixnum_value(sv[1]));
  return ary;
}

static mrb_value
mrb_socket_socket(mrb_state *mrb, mrb_value klass)
{
  mrb_int domain, type, protocol;
  int s;

  mrb_get_args(mrb, "iii", &domain, &type, &protocol);
  s = socket(domain, type, protocol);
  if (s == -1)
    mrb_sys_fail(mrb, "socket");
  return mrb_fixnum_value(s);
}

void
mrb_mruby_socket_gem_init(mrb_state* mrb)
{
  struct RClass *io, *ai, *sock, *bsock, *ipsock, *tcpsock, *udpsock, *usock;
  struct RClass *constants;

  ai = mrb_define_class(mrb, "Addrinfo", mrb->object_class);
  mrb_mod_cv_set(mrb, ai, mrb_intern_cstr(mrb, "_lastai"), mrb_nil_value());
  mrb_define_class_method(mrb, ai, "getaddrinfo", mrb_addrinfo_getaddrinfo, MRB_ARGS_REQ(2)|MRB_ARGS_OPT(4));
  mrb_define_method(mrb, ai, "getnameinfo", mrb_addrinfo_getnameinfo, MRB_ARGS_OPT(1));
  mrb_define_method(mrb, ai, "unix_path", mrb_addrinfo_unix_path, MRB_ARGS_NONE());

  io = mrb_class_get(mrb, "IO");

  bsock = mrb_define_class(mrb, "BasicSocket", io);
  mrb_define_method(mrb, bsock, "_recvfrom", mrb_basicsocket_recvfrom, MRB_ARGS_REQ(1)|MRB_ARGS_OPT(1));
  mrb_define_method(mrb, bsock, "_setnonblock", mrb_basicsocket_setnonblock, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, bsock, "getpeereid", mrb_basicsocket_getpeereid, MRB_ARGS_NONE());
  mrb_define_method(mrb, bsock, "getpeername", mrb_basicsocket_getpeername, MRB_ARGS_NONE());
  mrb_define_method(mrb, bsock, "getsockname", mrb_basicsocket_getsockname, MRB_ARGS_NONE());
  mrb_define_method(mrb, bsock, "getsockopt", mrb_basicsocket_getsockopt, MRB_ARGS_REQ(2));
  mrb_define_method(mrb, bsock, "recv", mrb_basicsocket_recv, MRB_ARGS_REQ(1)|MRB_ARGS_OPT(1));
  // #recvmsg(maxlen, flags=0)
  mrb_define_method(mrb, bsock, "send", mrb_basicsocket_send, MRB_ARGS_REQ(2)|MRB_ARGS_OPT(1));
  // #sendmsg
  // #sendmsg_nonblock
  mrb_define_method(mrb, bsock, "setsockopt", mrb_basicsocket_setsockopt, MRB_ARGS_REQ(1)|MRB_ARGS_OPT(2));
  mrb_define_method(mrb, bsock, "shutdown", mrb_basicsocket_shutdown, MRB_ARGS_OPT(1));

  ipsock = mrb_define_class(mrb, "IPSocket", bsock);
  mrb_define_class_method(mrb, ipsock, "ntop", mrb_ipsocket_ntop, MRB_ARGS_REQ(1));
  mrb_define_class_method(mrb, ipsock, "pton", mrb_ipsocket_pton, MRB_ARGS_REQ(2));
  mrb_define_method(mrb, ipsock, "recvfrom", mrb_ipsocket_recvfrom, MRB_ARGS_REQ(1)|MRB_ARGS_OPT(1));

  tcpsock = mrb_define_class(mrb, "TCPSocket", ipsock);
  //mrb_define_class_method(mrb, tcpsock, "open", mrb_tcpsocket_open, MRB_ARGS_REQ(2)|MRB_ARGS_OPT(2));
  //mrb_define_class_method(mrb, tcpsock, "new", mrb_tcpsocket_open, MRB_ARGS_REQ(2)|MRB_ARGS_OPT(2));
  mrb_define_class(mrb, "TCPServer", tcpsock);

  udpsock = mrb_define_class(mrb, "UDPSocket", ipsock);
  //#recvfrom_nonblock

  sock = mrb_define_class(mrb, "Socket", bsock);
  mrb_define_class_method(mrb, sock, "_accept", mrb_socket_accept, MRB_ARGS_REQ(1));
  mrb_define_class_method(mrb, sock, "_bind", mrb_socket_bind, MRB_ARGS_REQ(3));
  mrb_define_class_method(mrb, sock, "_connect", mrb_socket_connect, MRB_ARGS_REQ(3));
  mrb_define_class_method(mrb, sock, "_listen", mrb_socket_listen, MRB_ARGS_REQ(2));
  mrb_define_class_method(mrb, sock, "_sockaddr_family", mrb_socket_sockaddr_family, MRB_ARGS_REQ(1));
  mrb_define_class_method(mrb, sock, "_socket", mrb_socket_socket, MRB_ARGS_REQ(3));
  //mrb_define_class_method(mrb, sock, "gethostbyaddr", mrb_socket_gethostbyaddr, MRB_ARGS_REQ(1)|MRB_ARGS_OPT(1));
  //mrb_define_class_method(mrb, sock, "gethostbyname", mrb_socket_gethostbyname, MRB_ARGS_REQ(1)|MRB_ARGS_OPT(1));
  mrb_define_class_method(mrb, sock, "gethostname", mrb_socket_gethostname, MRB_ARGS_NONE());
  //mrb_define_class_method(mrb, sock, "getservbyname", mrb_socket_getservbyname, MRB_ARGS_REQ(1)|MRB_ARGS_OPT(1));
  //mrb_define_class_method(mrb, sock, "getservbyport", mrb_socket_getservbyport, MRB_ARGS_REQ(1)|MRB_ARGS_OPT(1));
  mrb_define_class_method(mrb, sock, "sockaddr_un", mrb_socket_sockaddr_un, MRB_ARGS_REQ(1));
  mrb_define_class_method(mrb, sock, "socketpair", mrb_socket_socketpair, MRB_ARGS_REQ(3));
  //mrb_define_method(mrb, sock, "sysaccept", mrb_socket_accept, MRB_ARGS_NONE());

  usock = mrb_define_class(mrb, "UNIXSocket", io);
  //mrb_define_class_method(mrb, usock, "pair", mrb_unixsocket_open, MRB_ARGS_OPT(2));
  //mrb_define_class_method(mrb, usock, "socketpair", mrb_unixsocket_open, MRB_ARGS_OPT(2));

  //mrb_define_method(mrb, usock, "recv_io", mrb_unixsocket_peeraddr, MRB_ARGS_NONE());
  //mrb_define_method(mrb, usock, "recvfrom", mrb_unixsocket_peeraddr, MRB_ARGS_NONE());
  //mrb_define_method(mrb, usock, "send_io", mrb_unixsocket_peeraddr, MRB_ARGS_NONE());

  constants = mrb_define_module_under(mrb, sock, "Constants");

#define define_const(SYM) \
  do {								\
    mrb_define_const(mrb, constants, #SYM, mrb_fixnum_value(SYM));	\
  } while (0)

#include "const.cstub"
}

void
mrb_mruby_socket_gem_final(mrb_state* mrb)
{
  mrb_value ai;
  ai = mrb_mod_cv_get(mrb, mrb_class_get(mrb, "Addrinfo"), mrb_intern_cstr(mrb, "_lastai"));
  if (mrb_voidp_p(ai)) {
    freeaddrinfo(mrb_voidp(ai));
  }
}
