/*
Copyright: Boaz segev, 2016-2017
License: MIT

Feel free to copy, use and enjoy according to the license provided.
*/
#include "iodine.h"

#include "http.h"

#include <ruby/encoding.h>
#include <ruby/io.h>
// #include "iodine_websockets.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

/* *****************************************************************************
Available Globals
***************************************************************************** */

typedef struct {
  VALUE app;
  VALUE env;
} iodine_http_settings_s;

/* these three are used also by iodin_rack_io.c */
VALUE IODINE_R_HIJACK;
VALUE IODINE_R_HIJACK_IO;
VALUE IODINE_R_HIJACK_CB;

static VALUE RACK_UPGRADE;
static VALUE RACK_UPGRADE_Q;
static VALUE RACK_UPGRADE_SSE;
static VALUE RACK_UPGRADE_WEBSOCKET;
static VALUE UPGRADE_TCP;

static VALUE hijack_func_sym;
static ID close_method_id;
static ID each_method_id;
static ID attach_method_id;
static ID iodine_to_s_method_id;
static ID iodine_call_proc_id;

static VALUE env_template_no_upgrade;
static VALUE env_template_websockets;
static VALUE env_template_sse;

static rb_encoding *IodineUTF8Encoding;
static rb_encoding *IodineBinaryEncoding;

static uint8_t support_xsendfile = 0;

/** Used by {listen2http} to set missing arguments. */
static VALUE iodine_default_args;

#define rack_declare(rack_name) static VALUE rack_name

#define rack_set(rack_name, str)                                               \
  (rack_name) = rb_enc_str_new((str), strlen((str)), IodineBinaryEncoding);    \
  rb_global_variable(&(rack_name));                                            \
  rb_obj_freeze(rack_name);
#define rack_set_sym(rack_name, sym)                                           \
  (rack_name) = rb_id2sym(rb_intern2((sym), strlen((sym))));                   \
  rb_global_variable(&(rack_name));

#define rack_autoset(rack_name) rack_set((rack_name), #rack_name)

// static uint8_t IODINE_IS_DEVELOPMENT_MODE = 0;

rack_declare(HTTP_SCHEME);
rack_declare(HTTPS_SCHEME);
rack_declare(QUERY_ESTRING);
rack_declare(REQUEST_METHOD);
rack_declare(PATH_INFO);
rack_declare(QUERY_STRING);
rack_declare(QUERY_ESTRING);
rack_declare(SERVER_NAME);
rack_declare(SERVER_PORT);
rack_declare(SERVER_PROTOCOL);
rack_declare(HTTP_VERSION);
rack_declare(REMOTE_ADDR);
rack_declare(CONTENT_LENGTH);
rack_declare(CONTENT_TYPE);
rack_declare(R_URL_SCHEME);          // rack.url_scheme
rack_declare(R_INPUT);               // rack.input
rack_declare(XSENDFILE);             // for X-Sendfile support
rack_declare(XSENDFILE_TYPE);        // for X-Sendfile support
rack_declare(XSENDFILE_TYPE_HEADER); // for X-Sendfile support
rack_declare(CONTENT_LENGTH_HEADER); // for X-Sendfile support

/* used internally to handle requests */
typedef struct {
  http_s *h;
  FIOBJ body;
  enum iodine_http_response_type_enum {
    IODINE_HTTP_NONE,
    IODINE_HTTP_SENDBODY,
    IODINE_HTTP_XSENDFILE,
    IODINE_HTTP_EMPTY,
    IODINE_HTTP_ERROR,
  } type;
  enum iodine_upgrade_type_enum {
    IODINE_UPGRADE_NONE = 0,
    IODINE_UPGRADE_WEBSOCKET,
    IODINE_UPGRADE_SSE,
  } upgrade;
} iodine_http_request_handle_s;

/* *****************************************************************************
WebSocket support
***************************************************************************** */

typedef struct {
  char *data;
  size_t size;
  uint8_t is_text;
  VALUE io;
} iodine_msg2ruby_s;

static void *iodine_ws_fire_message(void *msg_) {
  iodine_msg2ruby_s *msg = msg_;
  VALUE data = rb_enc_str_new(
      msg->data, msg->size,
      (msg->is_text ? rb_utf8_encoding() : rb_ascii8bit_encoding()));
  iodine_connection_fire_event(msg->io, IODINE_CONNECTION_ON_MESSAGE, data);
  return NULL;
}

static void iodine_ws_on_message(ws_s *ws, fio_str_info_s data,
                                 uint8_t is_text) {
  iodine_msg2ruby_s msg = {
      .data = data.data,
      .size = data.len,
      .is_text = is_text,
      .io = (VALUE)websocket_udata_get(ws),
  };
  IodineCaller.enterGVL(iodine_ws_fire_message, &msg);
}
/**
 * The (optional) on_open callback will be called once the websocket
 * connection is established and before is is registered with `facil`, so no
 * `on_message` events are raised before `on_open` returns.
 */
static void iodine_ws_on_open(ws_s *ws) {
  VALUE h = (VALUE)websocket_udata_get(ws);
  iodine_connection_s *c = iodine_connection_CData(h);
  c->arg = ws;
  c->uuid = websocket_uuid(ws);
  iodine_connection_fire_event(h, IODINE_CONNECTION_ON_OPEN, Qnil);
}
/**
 * The (optional) on_ready callback will be after a the underlying socket's
 * buffer changes it's state from full to empty.
 *
 * If the socket's buffer is never used, the callback is never called.
 */
static void iodine_ws_on_ready(ws_s *ws) {
  iodine_connection_fire_event((VALUE)websocket_udata_get(ws),
                               IODINE_CONNECTION_ON_DRAINED, Qnil);
}
/**
 * The (optional) on_shutdown callback will be called if a websocket
 * connection is still open while the server is shutting down (called before
 * `on_close`).
 */
static void iodine_ws_on_shutdown(ws_s *ws) {
  iodine_connection_fire_event((VALUE)websocket_udata_get(ws),
                               IODINE_CONNECTION_ON_SHUTDOWN, Qnil);
}
/**
 * The (optional) on_close callback will be called once a websocket connection
 * is terminated or failed to be established.
 *
 * The `uuid` is the connection's unique ID that can identify the Websocket. A
 * value of `uuid == 0` indicates the Websocket connection wasn't established
 * (an error occured).
 *
 * The `udata` is the user data as set during the upgrade or using the
 * `websocket_udata_set` function.
 */
static void iodine_ws_on_close(intptr_t uuid, void *udata) {
  iodine_connection_fire_event((VALUE)udata, IODINE_CONNECTION_ON_CLOSE, Qnil);
  (void)uuid;
}

static void iodine_ws_attach(http_s *h, VALUE handler, VALUE env) {
  VALUE io =
      iodine_connection_new(.type = IODINE_CONNECTION_WEBSOCKET, .arg = NULL,
                            .handler = handler, .env = env, .uuid = 0);
  if (io == Qnil)
    return;

  http_upgrade2ws(h, .on_message = iodine_ws_on_message,
                  .on_open = iodine_ws_on_open, .on_ready = iodine_ws_on_ready,
                  .on_shutdown = iodine_ws_on_shutdown,
                  .on_close = iodine_ws_on_close, .udata = (void *)io);
}

/* *****************************************************************************
SSE support
***************************************************************************** */

static void iodine_sse_on_ready(http_sse_s *sse) {
  iodine_connection_fire_event((VALUE)sse->udata, IODINE_CONNECTION_ON_DRAINED,
                               Qnil);
}

static void iodine_sse_on_shutdown(http_sse_s *sse) {
  iodine_connection_fire_event((VALUE)sse->udata, IODINE_CONNECTION_ON_SHUTDOWN,
                               Qnil);
}
static void iodine_sse_on_close(http_sse_s *sse) {
  iodine_connection_fire_event((VALUE)sse->udata, IODINE_CONNECTION_ON_CLOSE,
                               Qnil);
}

static void iodine_sse_on_open(http_sse_s *sse) {
  VALUE h = (VALUE)sse->udata;
  iodine_connection_s *c = iodine_connection_CData(h);
  c->arg = sse;
  c->uuid = http_sse2uuid(sse);
  iodine_connection_fire_event(h, IODINE_CONNECTION_ON_OPEN, Qnil);
  sse->on_ready = iodine_sse_on_ready;
  fio_force_event(c->uuid, FIO_EVENT_ON_READY);
}

static void iodine_sse_attach(http_s *h, VALUE handler, VALUE env) {
  VALUE io = iodine_connection_new(.type = IODINE_CONNECTION_SSE, .arg = NULL,
                                   .handler = handler, .env = env, .uuid = 0);
  if (io == Qnil)
    return;

  http_upgrade2sse(h, .on_open = iodine_sse_on_open,
                   .on_ready = NULL /* will be set after the on_open */,
                   .on_shutdown = iodine_sse_on_shutdown,
                   .on_close = iodine_sse_on_close, .udata = (void *)io);
}

/* *****************************************************************************
Copying data from the C request to the Rack's ENV
***************************************************************************** */

#define to_upper(c) (((c) >= 'a' && (c) <= 'z') ? ((c) & ~32) : (c))

int iodine_copy2env_task(FIOBJ o, void *env_) {
  VALUE env = (VALUE)env_;
  FIOBJ name = fiobj_hash_key_in_loop();
  fio_str_info_s tmp = fiobj_obj2cstr(name);
  VALUE hname = (VALUE)0;
  if (tmp.len > 59) {
    char *buf = fio_malloc(tmp.len + 5);
    memcpy(buf, "HTTP_", 5);
    for (size_t i = 0; i < tmp.len; ++i) {
      buf[i + 5] = (tmp.data[i] == '-') ? '_' : to_upper(tmp.data[i]);
    }
    hname = rb_enc_str_new(buf, tmp.len + 5, IodineBinaryEncoding);
    fio_free(buf);
  } else {
    char buf[64];
    memcpy(buf, "HTTP_", 5);
    for (size_t i = 0; i < tmp.len; ++i) {
      buf[i + 5] = (tmp.data[i] == '-') ? '_' : to_upper(tmp.data[i]);
    }
    hname = rb_enc_str_new(buf, tmp.len + 5, IodineBinaryEncoding);
  }

  if (FIOBJ_TYPE_IS(o, FIOBJ_T_STRING)) {
    tmp = fiobj_obj2cstr(o);
    rb_hash_aset(env, hname,
                 rb_enc_str_new(tmp.data, tmp.len, IodineBinaryEncoding));

  } else {
    /* it's an array */
    VALUE ary = rb_ary_new();
    rb_hash_aset(env, hname, ary);
    size_t count = fiobj_ary_count(o);
    for (size_t i = 0; i < count; ++i) {
      tmp = fiobj_obj2cstr(fiobj_ary_index(o, i));
      rb_ary_push(ary, rb_enc_str_new(tmp.data, tmp.len, IodineBinaryEncoding));
    }
  }
  return 0;
}
static inline VALUE copy2env(iodine_http_request_handle_s *handle) {
  VALUE env;
  http_s *h = handle->h;
  switch (handle->upgrade) {
  case IODINE_UPGRADE_WEBSOCKET:
    env = rb_hash_dup(env_template_websockets);
    break;
  case IODINE_UPGRADE_SSE:
    env = rb_hash_dup(env_template_sse);
    break;
  case IODINE_UPGRADE_NONE: /* fallthrough */
  default:
    env = rb_hash_dup(env_template_no_upgrade);
    break;
  }
  IodineStore.add(env);

  fio_str_info_s tmp;
  char *pos = NULL;
  /* Copy basic data */
  tmp = fiobj_obj2cstr(h->method);
  rb_hash_aset(env, REQUEST_METHOD,
               rb_enc_str_new(tmp.data, tmp.len, IodineBinaryEncoding));
  tmp = fiobj_obj2cstr(h->path);
  rb_hash_aset(env, PATH_INFO,
               rb_enc_str_new(tmp.data, tmp.len, IodineBinaryEncoding));
  if (h->query) {
    tmp = fiobj_obj2cstr(h->query);
    rb_hash_aset(env, QUERY_STRING,
                 tmp.len
                     ? rb_enc_str_new(tmp.data, tmp.len, IodineBinaryEncoding)
                     : QUERY_ESTRING);
  } else {
    rb_hash_aset(env, QUERY_STRING, QUERY_ESTRING);
  }
  {
    // HTTP version appears twice
    tmp = fiobj_obj2cstr(h->version);
    VALUE hname = rb_enc_str_new(tmp.data, tmp.len, IodineBinaryEncoding);
    rb_hash_aset(env, SERVER_PROTOCOL, hname);
    rb_hash_aset(env, HTTP_VERSION, hname);
  }

  { // Support for Ruby web-console.
    fio_str_info_s peer = http_peer_addr(h);
    if (peer.len) {
      rb_hash_aset(env, REMOTE_ADDR, rb_str_new(peer.data, peer.len));
    }
  }

  /* handle the HOST header, including the possible host:#### format*/
  static uint64_t host_hash = 0;
  if (!host_hash)
    host_hash = fio_siphash("host", 4);
  tmp = fiobj_obj2cstr(fiobj_hash_get2(h->headers, host_hash));
  pos = tmp.data;
  while (*pos && *pos != ':')
    pos++;
  if (*pos == 0) {
    rb_hash_aset(env, SERVER_NAME,
                 rb_enc_str_new(tmp.data, tmp.len, IodineBinaryEncoding));
    rb_hash_aset(env, SERVER_PORT, QUERY_ESTRING);
  } else {
    rb_hash_aset(
        env, SERVER_NAME,
        rb_enc_str_new(tmp.data, pos - tmp.data, IodineBinaryEncoding));
    ++pos;
    rb_hash_aset(
        env, SERVER_PORT,
        rb_enc_str_new(pos, tmp.len - (pos - tmp.data), IodineBinaryEncoding));
  }

  /* remove special headers */
  {
    static uint64_t content_length_hash = 0;
    if (!content_length_hash)
      content_length_hash = fio_siphash("content-length", 14);
    FIOBJ cl = fiobj_hash_get2(h->headers, content_length_hash);
    if (cl) {
      tmp = fiobj_obj2cstr(fiobj_hash_get2(h->headers, content_length_hash));
      if (tmp.data) {
        rb_hash_aset(env, CONTENT_LENGTH,
                     rb_enc_str_new(tmp.data, tmp.len, IodineBinaryEncoding));
        fiobj_hash_delete2(h->headers, content_length_hash);
      }
    }
  }
  {
    static uint64_t content_type_hash = 0;
    if (!content_type_hash)
      content_type_hash = fio_siphash("content-type", 12);
    FIOBJ ct = fiobj_hash_get2(h->headers, content_type_hash);
    if (ct) {
      tmp = fiobj_obj2cstr(ct);
      if (tmp.len && tmp.data) {
        fprintf(stderr, "Content type: %s\n", tmp.data);
        rb_hash_aset(env, CONTENT_TYPE,
                     rb_enc_str_new(tmp.data, tmp.len, IodineBinaryEncoding));
        fiobj_hash_delete2(h->headers, content_type_hash);
      }
    }
  }
  /* handle scheme / sepcial forwarding headers */
  {
    FIOBJ objtmp;
    static uint64_t xforward_hash = 0;
    if (!xforward_hash)
      xforward_hash = fio_siphash("x-forwarded-proto", 27);
    static uint64_t forward_hash = 0;
    if (!forward_hash)
      forward_hash = fio_siphash("forwarded", 9);
    if ((objtmp = fiobj_hash_get2(h->headers, xforward_hash))) {
      tmp = fiobj_obj2cstr(objtmp);
      if (tmp.len >= 5 && !strncasecmp(tmp.data, "https", 5)) {
        rb_hash_aset(env, R_URL_SCHEME, HTTPS_SCHEME);
      } else if (tmp.len == 4 && !strncasecmp(tmp.data, "http", 4)) {
        rb_hash_aset(env, R_URL_SCHEME, HTTP_SCHEME);
      } else {
        rb_hash_aset(env, R_URL_SCHEME,
                     rb_enc_str_new(tmp.data, tmp.len, IodineBinaryEncoding));
      }
    } else if ((objtmp = fiobj_hash_get2(h->headers, forward_hash))) {
      tmp = fiobj_obj2cstr(objtmp);
      pos = tmp.data;
      if (pos) {
        while (*pos) {
          if (((*(pos++) | 32) == 'p') && ((*(pos++) | 32) == 'r') &&
              ((*(pos++) | 32) == 'o') && ((*(pos++) | 32) == 't') &&
              ((*(pos++) | 32) == 'o') && ((*(pos++) | 32) == '=')) {
            if ((pos[0] | 32) == 'h' && (pos[1] | 32) == 't' &&
                (pos[2] | 32) == 't' && (pos[3] | 32) == 'p') {
              if ((pos[4] | 32) == 's') {
                rb_hash_aset(env, R_URL_SCHEME, HTTPS_SCHEME);
              } else {
                rb_hash_aset(env, R_URL_SCHEME, HTTP_SCHEME);
              }
            } else {
              char *tmp = pos;
              while (*tmp && *tmp != ';')
                tmp++;
              rb_hash_aset(env, R_URL_SCHEME, rb_str_new(pos, tmp - pos));
            }
            break;
          }
        }
      }
    } else {
    }
  }

  /* add all remianing headers */
  fiobj_each1(h->headers, 0, iodine_copy2env_task, (void *)env);
  return env;
}
#undef add_str_to_env
#undef add_value_to_env
#undef add_header_to_env

/* *****************************************************************************
Handling the HTTP response
***************************************************************************** */

// itterate through the headers and add them to the response buffer
// (we are recycling the request's buffer)
static int for_each_header_data(VALUE key, VALUE val, VALUE h_) {
  http_s *h = (http_s *)h_;
  // fprintf(stderr, "For_each - headers\n");
  if (TYPE(key) != T_STRING)
    key = IodineCaller.call(key, iodine_to_s_method_id);
  if (TYPE(key) != T_STRING)
    return ST_CONTINUE;
  if (TYPE(val) != T_STRING) {
    val = IodineCaller.call(val, iodine_to_s_method_id);
    if (TYPE(val) != T_STRING)
      return ST_STOP;
  }
  char *key_s = RSTRING_PTR(key);
  int key_len = RSTRING_LEN(key);
  char *val_s = RSTRING_PTR(val);
  int val_len = RSTRING_LEN(val);
  // make the headers lowercase
  FIOBJ name = fiobj_str_new(key_s, key_len);
  {
    fio_str_info_s tmp = fiobj_obj2cstr(name);
    for (int i = 0; i < key_len; ++i) {
      tmp.data[i] = tolower(tmp.data[i]);
    }
  }
  // scan the value for newline (\n) delimiters
  int pos_s = 0, pos_e = 0;
  while (pos_e < val_len) {
    // scanning for newline (\n) delimiters
    while (pos_e < val_len && val_s[pos_e] != '\n')
      pos_e++;
    http_set_header(h, name, fiobj_str_new(val_s + pos_s, pos_e - pos_s));
    // fprintf(stderr, "For_each - headers: wrote header\n");
    // move forward (skip the '\n' if exists)
    ++pos_e;
    pos_s = pos_e;
  }
  fiobj_free(name);
  // no errors, return 0
  return ST_CONTINUE;
}

// writes the body to the response object
static VALUE for_each_body_string(VALUE str, VALUE body_) {
  // fprintf(stderr, "For_each - body\n");
  // write body
  if (TYPE(str) != T_STRING) {
    fprintf(stderr, "Iodine Server Error:"
                    "response body was not a String\n");
    return Qfalse;
  }
  if (RSTRING_LEN(str) && RSTRING_PTR(str)) {
    fiobj_str_write((FIOBJ)body_, RSTRING_PTR(str), RSTRING_LEN(str));
  }
  return Qtrue;
}

static inline int ruby2c_response_send(iodine_http_request_handle_s *handle,
                                       VALUE rbresponse, VALUE env) {
  (void)(env);
  VALUE body = rb_ary_entry(rbresponse, 2);
  if (handle->h->status < 200 || handle->h->status == 204 ||
      handle->h->status == 304) {
    if (rb_respond_to(body, close_method_id))
      IodineCaller.call(body, close_method_id);
    body = Qnil;
    handle->type = IODINE_HTTP_NONE;
    return 0;
  }
  if (TYPE(body) == T_ARRAY) {
    if (RARRAY_LEN(body) == 0) { // only headers
      handle->type = IODINE_HTTP_EMPTY;
    } else if (RARRAY_LEN(body) == 1) { // [String] is likely
      body = rb_ary_entry(body, 0);
      // fprintf(stderr, "Body was a single item array, unpacket to string\n");
    }
  }

  if (TYPE(body) == T_STRING) {
    // fprintf(stderr, "Review body as String\n");
    if (RSTRING_LEN(body))
      handle->body = fiobj_str_new(RSTRING_PTR(body), RSTRING_LEN(body));
    handle->type = IODINE_HTTP_SENDBODY;
    return 0;
  } else if (rb_respond_to(body, each_method_id)) {
    // fprintf(stderr, "Review body as for-each ...\n");
    handle->body = fiobj_str_buf(1);
    handle->type = IODINE_HTTP_SENDBODY;
    rb_block_call(body, each_method_id, 0, NULL, for_each_body_string,
                  (VALUE)handle->body);
    // we need to call `close` in case the object is an IO / BodyProxy
    if (rb_respond_to(body, close_method_id))
      IodineCaller.call(body, close_method_id);
    return 0;
  }
  return -1;
}

/* *****************************************************************************
Handling Upgrade cases
***************************************************************************** */

static inline int ruby2c_review_upgrade(iodine_http_request_handle_s *req,
                                        VALUE rbresponse, VALUE env) {
  http_s *h = req->h;
  VALUE handler;
  if ((handler = rb_hash_aref(env, IODINE_R_HIJACK_CB)) != Qnil) {
    // send headers
    http_finish(h);
    // call the callback
    VALUE io_ruby = IodineCaller.call(rb_hash_aref(env, IODINE_R_HIJACK),
                                      iodine_call_proc_id);
    IodineCaller.call2(handler, iodine_call_proc_id, 1, &io_ruby);
    goto upgraded;
  } else if ((handler = rb_hash_aref(env, IODINE_R_HIJACK_IO)) != Qnil) {
    //  do nothing, just cleanup
    goto upgraded;
  } else if ((handler = rb_hash_aref(env, UPGRADE_TCP)) != Qnil) {
    goto tcp_ip_upgrade;
  } else {
    switch (req->upgrade) {
    case IODINE_UPGRADE_WEBSOCKET:
      if ((handler = rb_hash_aref(env, RACK_UPGRADE)) != Qnil) {
        // use response as existing base for native websocket upgrade
        iodine_ws_attach(h, handler, env);
        goto upgraded;
      }
      break;
    case IODINE_UPGRADE_SSE:
      if ((handler = rb_hash_aref(env, RACK_UPGRADE)) != Qnil) {
        // use response as existing base for SSE upgrade
        iodine_sse_attach(h, handler, env);
        goto upgraded;
      }
      break;
    default:
      if ((handler = rb_hash_aref(env, RACK_UPGRADE)) != Qnil) {
      tcp_ip_upgrade : {
        // use response as existing base for raw TCP/IP upgrade
        intptr_t uuid = http_hijack(h, NULL);
        // send headers
        http_finish(h);
        // upgrade protocol to raw TCP/IP
        iodine_tcp_attch_uuid(uuid, handler);
        goto upgraded;
      }
      }
      break;
    }
  }
  return 0;

upgraded:
  // get body object to close it (if needed)
  handler = rb_ary_entry(rbresponse, 2);
  // we need to call `close` in case the object is an IO / BodyProxy
  if (handler != Qnil && rb_respond_to(handler, close_method_id))
    IodineCaller.call(handler, close_method_id);
  return 1;
}

/* *****************************************************************************
Handling HTTP requests
***************************************************************************** */

static inline void *iodine_handle_request_in_GVL(void *handle_) {
  iodine_http_request_handle_s *handle = handle_;
  VALUE rbresponse = 0;
  VALUE env = 0;
  http_s *h = handle->h;
  if (!h->udata)
    goto err_not_found;

  // create / register env variable
  env = copy2env(handle);
  // create rack.io
  VALUE tmp = IodineRackIO.create(h, env);
  // pass env variable to handler
  rbresponse =
      IodineCaller.call2((VALUE)h->udata, iodine_call_proc_id, 1, &env);
  // close rack.io
  IodineRackIO.close(tmp);
  // test handler's return value
  if (rbresponse == 0 || rbresponse == Qnil)
    goto internal_error;
  IodineStore.add(rbresponse);

  // set response status
  tmp = rb_ary_entry(rbresponse, 0);
  if (TYPE(tmp) == T_STRING) {
    char *data = RSTRING_PTR(tmp);
    h->status = fio_atol(&data);
  } else if (TYPE(tmp) == T_FIXNUM) {
    h->status = FIX2ULONG(tmp);
  } else {
    goto internal_error;
  }

  // handle header copy from ruby land to C land.
  VALUE response_headers = rb_ary_entry(rbresponse, 1);
  if (TYPE(response_headers) != T_HASH)
    goto internal_error;
  // extract the X-Sendfile header (never show original path)
  // X-Sendfile support only present when iodine serves static files.
  VALUE xfiles;
  if (support_xsendfile &&
      (xfiles = rb_hash_aref(response_headers, XSENDFILE)) != Qnil &&
      TYPE(xfiles) == T_STRING) {
    if (OBJ_FROZEN(response_headers)) {
      response_headers = rb_hash_dup(response_headers);
    }
    IodineStore.add(response_headers);
    handle->body = fiobj_str_new(RSTRING_PTR(xfiles), RSTRING_LEN(xfiles));
    handle->type = IODINE_HTTP_XSENDFILE;
    rb_hash_delete(response_headers, XSENDFILE);
    // remove content length headers, as this will be controled by iodine
    rb_hash_delete(response_headers, CONTENT_LENGTH_HEADER);
    // review each header and write it to the response.
    rb_hash_foreach(response_headers, for_each_header_data, (VALUE)(h));
    IodineStore.remove(response_headers);
    // send the file directly and finish
    return NULL;
  }
  // review each header and write it to the response.
  rb_hash_foreach(response_headers, for_each_header_data, (VALUE)(h));
  // review for upgrade.
  if ((intptr_t)h->status < 300 &&
      ruby2c_review_upgrade(handle, rbresponse, env))
    goto external_done;
  // send the request body.
  if (ruby2c_response_send(handle, rbresponse, env))
    goto internal_error;

  IodineStore.remove(rbresponse);
  IodineStore.remove(env);
  return NULL;

external_done:
  IodineStore.remove(rbresponse);
  IodineStore.remove(env);
  handle->type = IODINE_HTTP_NONE;
  return NULL;

err_not_found:
  IodineStore.remove(rbresponse);
  IodineStore.remove(env);
  h->status = 404;
  handle->type = IODINE_HTTP_ERROR;
  return NULL;

internal_error:
  IodineStore.remove(rbresponse);
  IodineStore.remove(env);
  h->status = 500;
  handle->type = IODINE_HTTP_ERROR;
  return NULL;
}

static inline void
iodine_perform_handle_action(iodine_http_request_handle_s handle) {
  switch (handle.type) {
  case IODINE_HTTP_SENDBODY: {
    fio_str_info_s data = fiobj_obj2cstr(handle.body);
    http_send_body(handle.h, data.data, data.len);
    fiobj_free(handle.body);
    break;
  }
  case IODINE_HTTP_XSENDFILE: {
    /* remove chunked content-encoding header, if any (Rack issue #1266) */
    if (fiobj_obj2cstr(
            fiobj_hash_get2(handle.h->private_data.out_headers,
                            fiobj_obj2hash(HTTP_HEADER_CONTENT_ENCODING)))
            .len == 7)
      fiobj_hash_delete2(handle.h->private_data.out_headers,
                         fiobj_obj2hash(HTTP_HEADER_CONTENT_ENCODING));
    fio_str_info_s data = fiobj_obj2cstr(handle.body);
    if (http_sendfile2(handle.h, data.data, data.len, NULL, 0)) {
      http_send_error(handle.h, 404);
    }
    fiobj_free(handle.body);
    break;
  }
  case IODINE_HTTP_EMPTY:
    http_finish(handle.h);
    fiobj_free(handle.body);
    break;
  case IODINE_HTTP_NONE:
    /* nothing to do - this had to be performed within the Ruby GIL :-( */
    break;
  case IODINE_HTTP_ERROR:
    http_send_error(handle.h, handle.h->status);
    fiobj_free(handle.body);
    break;
  }
}
static void on_rack_request(http_s *h) {
  iodine_http_request_handle_s handle = (iodine_http_request_handle_s){
      .h = h,
      .upgrade = IODINE_UPGRADE_NONE,
  };
  IodineCaller.enterGVL((void *(*)(void *))iodine_handle_request_in_GVL,
                        &handle);
  iodine_perform_handle_action(handle);
}

static void on_rack_upgrade(http_s *h, char *proto, size_t len) {
  iodine_http_request_handle_s handle = (iodine_http_request_handle_s){.h = h};
  if (len == 9 && (proto[1] == 'e' || proto[1] == 'E')) {
    handle.upgrade = IODINE_UPGRADE_WEBSOCKET;
  } else if (len == 3 && proto[0] == 's') {
    handle.upgrade = IODINE_UPGRADE_SSE;
  }
  /* when we stop supporting custom Upgrade headers: */
  // else {
  //   http_send_error(h, 400);
  //   return;
  // }
  IodineCaller.enterGVL(iodine_handle_request_in_GVL, &handle);
  iodine_perform_handle_action(handle);
  (void)proto;
  (void)len;
}

/* *****************************************************************************
Listenninng to HTTP
*****************************************************************************
*/

void *iodine_print_http_msg_in_gvl(void *d_) {
  // Write message
  struct {
    VALUE www;
    VALUE port;
  } *arg = d_;
  if (arg->www) {
    fprintf(stderr,
            "Iodine HTTP Server on port %s:\n"
            " *    Serving static files from %s\n\n",
            (arg->port ? StringValueCStr(arg->port) : "----"),
            StringValueCStr(arg->www));
  }
  return NULL;
}

static void iodine_print_http_msg(void *www, void *port) {
  if (!fio_is_master())
    goto finish;
  struct {
    void *www;
    void *port;
  } data = {.www = www, .port = port};
  IodineCaller.enterGVL(iodine_print_http_msg_in_gvl, (void *)&data);
finish:
  if (www) {
    IodineStore.remove((VALUE)www);
  }
  IodineStore.remove((VALUE)port);
}

static void free_iodine_http(http_settings_s *s) {
  IodineStore.remove((VALUE)s->udata);
}

// clang-format off
/**
Listens to incoming HTTP connections and handles incoming requests using the
Rack specification.

This is delegated to a lower level C HTTP and Websocket implementation, no
Ruby object will be crated except the `env` object required by the Rack
specifications.

Accepts a single Hash argument with the following properties:

(it's possible to set default values using the {Iodine::DEFAULT_HTTP_ARGS} Hash)

app:: the Rack application that handles incoming requests. Default: `nil`.
port:: the port to listen to. Default: 3000.
address:: the address to bind to. Default: binds to all possible addresses.
log:: enable response logging (Hijacked sockets aren't logged). Default: off.
public:: The root public folder for static file service. Default: none.
timeout:: Timeout for inactive HTTP/1.x connections. Defaults: 40 seconds.
max_body:: The maximum body size for incoming HTTP messages. Default: ~50Mib.
max_headers:: The maximum total header length for incoming HTTP messages. Default: ~64Kib.
max_msg:: The maximum Websocket message size allowed. Default: ~250Kib.
ping:: The Websocket `ping` interval. Default: 40 seconds.

Either the `app` or the `public` properties are required. If niether exists,
the function will fail. If both exist, Iodine will serve static files as well
as dynamic requests.

When using the static file server, it's possible to serve `gzip` versions of
the static files by saving a compressed version with the `gz` extension (i.e.
`styles.css.gz`).

`gzip` will only be served to clients tat support the `gzip` transfer
encoding.

Once HTTP/2 is supported (planned, but probably very far away), HTTP/2
timeouts will be dynamically managed by Iodine. The `timeout` option is only
relevant to HTTP/1.x connections.
*/
VALUE iodine_http_listen(VALUE self, VALUE opt) {
  // clang-format on
  uint8_t log_http = 0;
  size_t ping = 0;
  size_t max_body = 0;
  size_t max_headers = 0;
  size_t max_msg = 0;
  Check_Type(opt, T_HASH);
  /* copy from deafult hash */
  /* test arguments */
  VALUE app = rb_hash_aref(opt, ID2SYM(rb_intern("app")));
  VALUE www = rb_hash_aref(opt, ID2SYM(rb_intern("public")));
  VALUE port = rb_hash_aref(opt, ID2SYM(rb_intern("port")));
  VALUE address = rb_hash_aref(opt, ID2SYM(rb_intern("address")));
  VALUE tout = rb_hash_aref(opt, ID2SYM(rb_intern("timeout")));
  if (www == Qnil) {
    www = rb_hash_aref(iodine_default_args, ID2SYM(rb_intern("public")));
  }
  if (port == Qnil) {
    port = rb_hash_aref(iodine_default_args, ID2SYM(rb_intern("port")));
  }
  if (address == Qnil) {
    address = rb_hash_aref(iodine_default_args, ID2SYM(rb_intern("address")));
  }
  if (tout == Qnil) {
    tout = rb_hash_aref(iodine_default_args, ID2SYM(rb_intern("timeout")));
  }

  VALUE tmp = rb_hash_aref(opt, ID2SYM(rb_intern("max_msg")));
  if (tmp == Qnil) {
    tmp = rb_hash_aref(iodine_default_args, ID2SYM(rb_intern("max_msg")));
  }
  if (tmp != Qnil && tmp != Qfalse) {
    Check_Type(tmp, T_FIXNUM);
    max_msg = FIX2ULONG(tmp);
  }

  tmp = rb_hash_aref(opt, ID2SYM(rb_intern("max_body")));
  if (tmp == Qnil) {
    tmp = rb_hash_aref(iodine_default_args, ID2SYM(rb_intern("max_body")));
  }
  if (tmp != Qnil && tmp != Qfalse) {
    Check_Type(tmp, T_FIXNUM);
    max_body = FIX2ULONG(tmp);
  }
  tmp = rb_hash_aref(opt, ID2SYM(rb_intern("max_headers")));
  if (tmp == Qnil) {
    tmp = rb_hash_aref(iodine_default_args, ID2SYM(rb_intern("max_headers")));
  }
  if (tmp != Qnil && tmp != Qfalse) {
    Check_Type(tmp, T_FIXNUM);
    max_headers = FIX2ULONG(tmp);
  }

  tmp = rb_hash_aref(opt, ID2SYM(rb_intern("ping")));
  if (tmp == Qnil) {
    tmp = rb_hash_aref(iodine_default_args, ID2SYM(rb_intern("ping")));
  }
  if (tmp != Qnil && tmp != Qfalse) {
    Check_Type(tmp, T_FIXNUM);
    ping = FIX2ULONG(tmp);
  }
  if (ping > 255) {
    fprintf(stderr, "Iodine Warning: Websocket timeout value "
                    "is over 255 and will be ignored.\n");
    ping = 0;
  }

  tmp = rb_hash_aref(opt, ID2SYM(rb_intern("log")));
  if (tmp == Qnil) {
    tmp = rb_hash_aref(iodine_default_args, ID2SYM(rb_intern("log")));
  }
  if (tmp != Qnil && tmp != Qfalse)
    log_http = 1;

  if ((app == Qnil || app == Qfalse) && (www == Qnil || www == Qfalse)) {
    fprintf(stderr, "Iodine Warning: HTTP without application or public folder "
                    "(ignored).\n");
    return Qfalse;
  }

  if ((www != Qnil && www != Qfalse)) {
    Check_Type(www, T_STRING);
    IodineStore.add(www);
    rb_hash_aset(env_template_no_upgrade, XSENDFILE_TYPE, XSENDFILE);
    rb_hash_aset(env_template_no_upgrade, XSENDFILE_TYPE_HEADER, XSENDFILE);
    support_xsendfile = 1;
  } else
    www = 0;

  if ((address != Qnil && address != Qfalse))
    Check_Type(address, T_STRING);
  else
    address = 0;

  if ((tout != Qnil && tout != Qfalse)) {
    Check_Type(tout, T_FIXNUM);
    tout = FIX2ULONG(tout);
  } else
    tout = 0;
  if (tout > 255) {
    fprintf(stderr, "Iodine Warning: HTTP timeout value "
                    "is over 255 and is silently ignored.\n");
    tout = 0;
  }

  if (port != Qnil && port != Qfalse) {
    if (!RB_TYPE_P(port, T_STRING) && !RB_TYPE_P(port, T_FIXNUM))
      rb_raise(rb_eTypeError,
               "The `port` property MUST be either a String or a Number");
    if (RB_TYPE_P(port, T_FIXNUM))
      port = rb_funcall2(port, iodine_to_s_method_id, 0, NULL);
    IodineStore.add(port);
  } else if (port == Qfalse)
    port = 0;
  else {
    port = rb_str_new("3000", 4);
    IodineStore.add(port);
  }

  if ((app != Qnil && app != Qfalse))
    IodineStore.add(app);
  else
    app = 0;

  if (http_listen(
          StringValueCStr(port), (address ? StringValueCStr(address) : NULL),
          .on_request = on_rack_request, .on_upgrade = on_rack_upgrade,
          .udata = (void *)app, .timeout = (tout ? FIX2INT(tout) : tout),
          .ws_timeout = ping, .ws_max_msg_size = max_msg,
          .max_header_size = max_headers, .on_finish = free_iodine_http,
          .log = log_http, .max_body_size = max_body,
          .public_folder = (www ? StringValueCStr(www) : NULL))) {
    fprintf(stderr,
            "ERROR: Failed to initialize a listening HTTP socket for port %s\n",
            port ? StringValueCStr(port) : "3000");
    return Qfalse;
  }

  if ((app == Qnil || app == Qfalse)) {
    fprintf(stderr,
            "* Iodine: (no app) the HTTP service on port %s will only serve "
            "static files.\n",
            (port ? StringValueCStr(port) : "3000"));
  }
  fio_defer(iodine_print_http_msg, (www ? (void *)www : NULL), (void *)port);

  return Qtrue;
  (void)self;
}

static void initialize_env_template(void) {
  if (env_template_no_upgrade)
    return;
  env_template_no_upgrade = rb_hash_new();
  IodineStore.add(env_template_no_upgrade);

#define add_str_to_env(env, key, value)                                        \
  {                                                                            \
    VALUE k = rb_enc_str_new((key), strlen((key)), IodineBinaryEncoding);      \
    rb_obj_freeze(k);                                                          \
    VALUE v = rb_enc_str_new((value), strlen((value)), IodineBinaryEncoding);  \
    rb_obj_freeze(v);                                                          \
    rb_hash_aset(env, k, v);                                                   \
  }
#define add_value_to_env(env, key, value)                                      \
  {                                                                            \
    VALUE k = rb_enc_str_new((key), strlen((key)), IodineBinaryEncoding);      \
    rb_obj_freeze(k);                                                          \
    rb_hash_aset((env), k, value);                                             \
  }

  /* Set global template */
  rb_hash_aset(env_template_no_upgrade, RACK_UPGRADE_Q, Qnil);
  rb_hash_aset(env_template_no_upgrade, RACK_UPGRADE, Qnil);
  {
    /* add the rack.version */
    static VALUE rack_version = 0;
    if (!rack_version) {
      rack_version = rb_ary_new(); // rb_ary_new is Ruby 2.0 compatible
      rb_ary_push(rack_version, INT2FIX(1));
      rb_ary_push(rack_version, INT2FIX(3));
      rb_global_variable(&rack_version);
      rb_ary_freeze(rack_version);
    }
    add_value_to_env(env_template_no_upgrade, "rack.version", rack_version);
  }
  add_str_to_env(env_template_no_upgrade, "SCRIPT_NAME", "");
  add_value_to_env(env_template_no_upgrade, "rack.errors", rb_stderr);
  add_value_to_env(env_template_no_upgrade, "rack.hijack?", Qtrue);
  add_value_to_env(env_template_no_upgrade, "rack.multiprocess", Qtrue);
  add_value_to_env(env_template_no_upgrade, "rack.multithread", Qtrue);
  add_value_to_env(env_template_no_upgrade, "rack.run_once", Qfalse);
  /* default schema to http, it might be updated later */
  rb_hash_aset(env_template_no_upgrade, R_URL_SCHEME, HTTP_SCHEME);
  /* placeholders... minimize rehashing*/
  rb_hash_aset(env_template_no_upgrade, HTTP_VERSION, QUERY_STRING);
  rb_hash_aset(env_template_no_upgrade, IODINE_R_HIJACK, QUERY_STRING);
  rb_hash_aset(env_template_no_upgrade, PATH_INFO, QUERY_STRING);
  rb_hash_aset(env_template_no_upgrade, QUERY_STRING, QUERY_STRING);
  rb_hash_aset(env_template_no_upgrade, REMOTE_ADDR, QUERY_STRING);
  rb_hash_aset(env_template_no_upgrade, REQUEST_METHOD, QUERY_STRING);
  rb_hash_aset(env_template_no_upgrade, SERVER_NAME, QUERY_STRING);
  rb_hash_aset(env_template_no_upgrade, SERVER_PORT, QUERY_ESTRING);
  rb_hash_aset(env_template_no_upgrade, SERVER_PROTOCOL, QUERY_STRING);

  /* WebSocket upgrade support */
  env_template_websockets = rb_hash_dup(env_template_no_upgrade);
  IodineStore.add(env_template_websockets);
  rb_hash_aset(env_template_websockets, RACK_UPGRADE_Q, RACK_UPGRADE_WEBSOCKET);

  /* SSE upgrade support */
  env_template_sse = rb_hash_dup(env_template_no_upgrade);
  IodineStore.add(env_template_sse);
  rb_hash_aset(env_template_sse, RACK_UPGRADE_Q, RACK_UPGRADE_SSE);

#undef add_value_to_env
#undef add_str_to_env
}

/* *****************************************************************************
Initialization
***************************************************************************** */

void iodine_init_http(void) {

  rb_define_module_function(IodineModule, "listen2http", iodine_http_listen, 1);

  /** Used by {listen2http} to set missing arguments. */
  iodine_default_args = rb_hash_new();

  /** Used by {listen2http} to set missing arguments. */
  rb_const_set(IodineModule, rb_intern2("DEFAULT_HTTP_ARGS", 17),
               iodine_default_args);

  rack_autoset(REQUEST_METHOD);
  rack_autoset(PATH_INFO);
  rack_autoset(QUERY_STRING);
  rack_autoset(SERVER_NAME);
  rack_autoset(SERVER_PORT);
  rack_autoset(CONTENT_LENGTH);
  rack_autoset(CONTENT_TYPE);
  rack_autoset(SERVER_PROTOCOL);
  rack_autoset(HTTP_VERSION);
  rack_autoset(REMOTE_ADDR);
  rack_set(HTTP_SCHEME, "http");
  rack_set(HTTPS_SCHEME, "https");
  rack_set(QUERY_ESTRING, "");
  rack_set(R_URL_SCHEME, "rack.url_scheme");
  rack_set(R_INPUT, "rack.input");
  rack_set(XSENDFILE, "X-Sendfile");
  rack_set(XSENDFILE_TYPE, "sendfile.type");
  rack_set(XSENDFILE_TYPE_HEADER, "HTTP_X_SENDFILE_TYPE");
  rack_set(CONTENT_LENGTH_HEADER, "Content-Length");

  rack_set(IODINE_R_HIJACK_IO, "rack.hijack_io");
  rack_set(IODINE_R_HIJACK, "rack.hijack");
  rack_set(IODINE_R_HIJACK_CB, "iodine.hijack_cb");

  rack_set(RACK_UPGRADE, "rack.upgrade");
  rack_set(RACK_UPGRADE_Q, "rack.upgrade?");
  rack_set_sym(RACK_UPGRADE_SSE, "sse");
  rack_set_sym(RACK_UPGRADE_WEBSOCKET, "websocket");

  UPGRADE_TCP = IodineStore.add(rb_str_new("upgrade.tcp", 11));

  hijack_func_sym = ID2SYM(rb_intern("_hijack"));
  close_method_id = rb_intern("close");
  each_method_id = rb_intern("each");
  attach_method_id = rb_intern("attach_fd");
  iodine_to_s_method_id = rb_intern("to_s");
  iodine_call_proc_id = rb_intern("call");

  IodineUTF8Encoding = rb_enc_find("UTF-8");
  IodineBinaryEncoding = rb_enc_find("binary");

  initialize_env_template();
}
