#define _GNU_SOURCE
#include "forge_web.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <microhttpd.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define H(p) ((int64_t)(intptr_t)(p))
#define P(t, h) ((t *)(intptr_t)(h))
#define BODY_MAX (25u * 1024u * 1024u)
#define JSON_MAX (2u * 1024u * 1024u)
#define FILE_MAX (20u * 1024u * 1024u)
#define FETCH_MAX (4u * 1024u * 1024u)
typedef struct Owned {
  void *value;
  int json;
  struct Owned *next;
} Owned;
static _Thread_local Owned *owned;
static void *track(void *value, int json) {
  if (!value)
    return NULL;
  Owned *o = malloc(sizeof(*o));
  if (!o) {
    if (json)
      json_object_put(value);
    else
      free(value);
    return NULL;
  }
  o->value = value;
  o->json = json;
  o->next = owned;
  owned = o;
  return value;
}
static const char *copy(const char *s) { return track(strdup(s ? s : ""), 0); }
int64_t fw_scope_begin(void) { return fw_scope_end(); }
int64_t fw_scope_end(void) {
  while (owned) {
    Owned *o = owned;
    owned = o->next;
    if (o->json)
      json_object_put(o->value);
    else
      free(o->value);
    free(o);
  }
  return 1;
}
const char *fw_env(const char *name, const char *fallback) {
  const char *s = getenv(name);
  return s ? s : fallback;
}
int64_t fw_parse(const char *text) {
  if (!text || strlen(text) > FETCH_MAX)
    return 0;
  struct json_tokener *tok = json_tokener_new_ex(32);
  if (!tok)
    return 0;
  json_tokener_set_flags(tok, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
  size_t n = strlen(text);
  struct json_object *v = json_tokener_parse_ex(tok, text, (int)n + 1);
  size_t end = json_tokener_get_parse_end(tok);
  while (end < n && isspace((unsigned char)text[end]))
    end++;
  int ok = json_tokener_get_error(tok) == json_tokener_success && end == n;
  json_tokener_free(tok);
  if (!ok) {
    if (v)
      json_object_put(v);
    return 0;
  }
  return H(track(v, 1));
}
int64_t fw_kind(int64_t h) {
  if (!h)
    return 0;
  switch (json_object_get_type(P(struct json_object, h))) {
  case json_type_null:
    return 0;
  case json_type_boolean:
    return 1;
  case json_type_int:
    return 2;
  case json_type_string:
    return 3;
  case json_type_object:
    return 4;
  case json_type_array:
    return 5;
  case json_type_double:
    return 6;
  }
  return 0;
}
int64_t fw_get(int64_t h, const char *key) {
  struct json_object *v = NULL;
  if (fw_kind(h) != 4 || !key)
    return 0;
  json_object_object_get_ex(P(struct json_object, h), key, &v);
  return H(v);
}
int64_t fw_at(int64_t h, int64_t i) {
  return fw_kind(h) == 5 && i >= 0 &&
                 (uint64_t)i <
                     json_object_array_length(P(struct json_object, h))
             ? H(json_object_array_get_idx(P(struct json_object, h), i))
             : 0;
}
int64_t fw_count(int64_t h) {
  return fw_kind(h) == 5
             ? (int64_t)json_object_array_length(P(struct json_object, h))
         : fw_kind(h) == 4 ? json_object_object_length(P(struct json_object, h))
                           : 0;
}
const char *fw_text(int64_t h) {
  return fw_kind(h) == 3 ? json_object_get_string(P(struct json_object, h))
                         : "";
}
int64_t fw_integer(int64_t h) {
  return fw_kind(h) == 2 ? json_object_get_int64(P(struct json_object, h)) : 0;
}
int64_t fw_boolean(int64_t h) {
  return fw_kind(h) == 1 ? json_object_get_boolean(P(struct json_object, h))
                         : 0;
}
const char *fw_dump(int64_t h) {
  return h ? json_object_to_json_string_ext(P(struct json_object, h),
                                            JSON_C_TO_STRING_PLAIN)
           : "null";
}
int64_t fw_object(void) { return H(track(json_object_new_object(), 1)); }
int64_t fw_array(void) { return H(track(json_object_new_array(), 1)); }
int64_t fw_string(const char *v) {
  return H(track(json_object_new_string(v ? v : ""), 1));
}
int64_t fw_number(int64_t v) { return H(track(json_object_new_int64(v), 1)); }
int64_t fw_bool(int64_t v) {
  return H(track(json_object_new_boolean(v != 0), 1));
}
int64_t fw_set(int64_t h, const char *key, int64_t v) {
  if (fw_kind(h) != 4 || !key)
    return 0;
  return json_object_object_add(P(struct json_object, h), key,
                                json_object_get(P(struct json_object, v))) == 0;
}
int64_t fw_push(int64_t h, int64_t v) {
  return fw_kind(h) == 5 &&
         json_object_array_add(P(struct json_object, h),
                               json_object_get(P(struct json_object, v))) == 0;
}
int64_t fw_keys(int64_t h) {
  int64_t a = fw_array();
  if (fw_kind(h) == 4) {
    json_object_object_foreach(P(struct json_object, h), key, value) {
      (void)value;
      fw_push(a, fw_string(key));
    }
  }
  return a;
}
const char *fw_trim(const char *s) {
  if (!s)
    return "";
  while (isspace((unsigned char)*s))
    s++;
  size_t n = strlen(s);
  while (n && isspace((unsigned char)s[n - 1]))
    n--;
  return track(strndup(s, n), 0);
}
int64_t fw_chars(const char *s) {
  int64_t n = 0;
  if (s)
    while (*s) {
      if (((unsigned char)*s & 0xc0) != 0x80)
        n++;
      s++;
    }
  return n;
}
const char *fw_lower(const char *s) {
  char *v = (char *)copy(s);
  if (v)
    for (char *p = v; *p; p++)
      *p = (char)tolower((unsigned char)*p);
  return v ? v : "";
}
int64_t fw_equal(const char *a, const char *b) {
  return a && b && strcmp(a, b) == 0;
}
int64_t fw_starts(const char *s, const char *prefix) {
  return s && prefix && strncmp(s, prefix, strlen(prefix)) == 0;
}
const char *fw_part(const char *s, int64_t index) {
  if (!s || index < 0)
    return "";
  while (*s == '/')
    s++;
  for (int64_t i = 0; i < index; i++) {
    s = strchr(s, '/');
    if (!s)
      return "";
    s++;
  }
  const char *end = strchr(s, '/');
  return track(strndup(s, end ? (size_t)(end - s) : strlen(s)), 0);
}
static int private_ip(const char *s) {
  struct in_addr a;
  struct in6_addr b;
  if (inet_pton(AF_INET, s, &a) == 1) {
    uint32_t v = ntohl(a.s_addr);
    return (v >> 24) == 127 || (v >> 24) == 10 || (v >> 24) == 0 ||
           (v >> 16) == 0xa9fe || (v >> 20) == 0xac1 || (v >> 16) == 0xc0a8 ||
           (v >> 28) >= 14;
  }
  if (inet_pton(AF_INET6, s, &b) == 1) {
    if (IN6_IS_ADDR_V4MAPPED(&b)) {
      char mapped[INET_ADDRSTRLEN];
      inet_ntop(AF_INET, &b.s6_addr[12], mapped, sizeof(mapped));
      return private_ip(mapped);
    }
    return IN6_IS_ADDR_LOOPBACK(&b) || IN6_IS_ADDR_UNSPECIFIED(&b) ||
           (b.s6_addr[0] & 0xfe) == 0xfc ||
           (b.s6_addr[0] == 0xfe && (b.s6_addr[1] & 0xc0) == 0x80) ||
           b.s6_addr[0] == 0xff;
  }
  return 1;
}
int64_t fw_valid(const char *s, const char *kind) {
  if (!s || !kind)
    return 0;
  size_t n = strlen(s);
  if (!strcmp(kind, "uuid")) {
    if (n != 36)
      return 0;
    for (size_t i = 0; i < n; i++)
      if ((i == 8 || i == 13 || i == 18 || i == 23)
              ? s[i] != '-'
              : !isxdigit((unsigned char)s[i]))
        return 0;
    return 1;
  }
  if (!strcmp(kind, "id")) {
    if (!n || n > 19)
      return 0;
    for (size_t i = 0; i < n; i++)
      if (!isdigit((unsigned char)s[i]))
        return 0;
    errno = 0;
    long long v = strtoll(s, NULL, 10);
    return !errno && v > 0;
  }
  if (!strcmp(kind, "login")) {
    if (!n || n > 39)
      return 0;
    for (size_t i = 0; i < n; i++)
      if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
            (s[i] >= '0' && s[i] <= '9') || s[i] == '-'))
        return 0;
    return 1;
  }
  if (!strcmp(kind, "github_part")) {
    if (!n || n > 100 || s[0] == '.')
      return 0;
    for (size_t i = 0; i < n; i++)
      if (!(isalnum((unsigned char)s[i]) || s[i] == '-' || s[i] == '_' ||
            s[i] == '.'))
        return 0;
    return 1;
  }
  if (!strcmp(kind, "email")) {
    const char *at = strchr(s, '@');
    if (!at || at == s || !at[1] || strchr(at + 1, '@') || !strchr(at + 1, '.'))
      return 0;
    for (size_t i = 0; i < n; i++)
      if (isspace((unsigned char)s[i]) || iscntrl((unsigned char)s[i]))
        return 0;
    return 1;
  }
  if (!strcmp(kind, "url")) {
    CURLU *u = curl_url();
    if (!u)
      return 0;
    int ok = curl_url_set(u, CURLUPART_URL, s, 0) == CURLUE_OK;
    char *scheme = NULL, *host = NULL;
    if (ok)
      ok = curl_url_get(u, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
           curl_url_get(u, CURLUPART_HOST, &host, 0) == CURLUE_OK &&
           (!strcasecmp(scheme, "http") || !strcasecmp(scheme, "https")) &&
           *host;
    curl_free(scheme);
    curl_free(host);
    curl_url_cleanup(u);
    return ok;
  }
  if (!strcmp(kind, "return_path")) {
    if (!n || s[0] != '/' || s[1] == '/')
      return 0;
    for (size_t i = 0; i < n; i++)
      if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
            isdigit((unsigned char)s[i]) || strchr("/-_.~", s[i])))
        return 0;
    return 1;
  }
  if (!strcmp(kind, "ip") || !strcmp(kind, "public_ip")) {
    struct in_addr a;
    struct in6_addr b;
    int ok = inet_pton(AF_INET, s, &a) == 1 || inet_pton(AF_INET6, s, &b) == 1;
    return ok && (strcmp(kind, "public_ip") || !private_ip(s));
  }
  if (!strcmp(kind, "filename")) {
    if (!n || n > 100 || s[0] == '.')
      return 0;
    for (size_t i = 0; i < n; i++)
      if (!(isalnum((unsigned char)s[i]) || s[i] == '-' || s[i] == '.'))
        return 0;
    return 1;
  }
  return 0;
}
const char *fw_uuid(void) {
  unsigned char b[16];
  if (RAND_bytes(b, 16) != 1)
    return "";
  b[6] = (b[6] & 15) | 64;
  b[8] = (b[8] & 63) | 128;
  char s[37];
  snprintf(
      s, sizeof(s),
      "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
      b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11],
      b[12], b[13], b[14], b[15]);
  return copy(s);
}
int64_t fw_now(void) { return time(NULL); }
typedef struct {
  char key[256];
  time_t start;
  int count;
} Rate;
static Rate rates[4096];
static pthread_mutex_t rate_mutex = PTHREAD_MUTEX_INITIALIZER;
int64_t fw_rate(const char *key, int64_t max, int64_t seconds) {
  if (!key || strlen(key) >= sizeof(rates[0].key) || max < 1 || seconds < 1)
    return 0;
  time_t now = time(NULL);
  pthread_mutex_lock(&rate_mutex);
  int slot = -1;
  time_t oldest = now;
  for (int i = 0; i < 4096; i++) {
    if (!strcmp(rates[i].key, key)) {
      slot = i;
      break;
    }
    if (!rates[i].key[0] || rates[i].start < oldest) {
      oldest = rates[i].start;
      slot = i;
    }
  }
  if (slot < 0) {
    pthread_mutex_unlock(&rate_mutex);
    return 0;
  }
  Rate *r = &rates[slot];
  if (strcmp(r->key, key) || now - r->start >= seconds) {
    snprintf(r->key, sizeof(r->key), "%s", key);
    r->start = now;
    r->count = 0;
  }
  int ok = r->count < max;
  if (ok)
    r->count++;
  pthread_mutex_unlock(&rate_mutex);
  return ok;
}
const char *fw_urlencode(const char *s) {
  CURL *c = curl_easy_init();
  if (!c)
    return "";
  char *e = curl_easy_escape(c, s ? s : "", 0);
  const char *out = copy(e);
  curl_free(e);
  curl_easy_cleanup(c);
  return out ? out : "";
}
typedef struct {
  char *data;
  size_t size;
} Buffer;
static size_t receive_http(char *data, size_t size, size_t count, void *cls) {
  Buffer *b = cls;
  if (size && count > SIZE_MAX / size)
    return 0;
  size_t n = size * count;
  if (n > FETCH_MAX - b->size)
    return 0;
  char *p = realloc(b->data, b->size + n + 1);
  if (!p)
    return 0;
  b->data = p;
  memcpy(p + b->size, data, n);
  b->size += n;
  p[b->size] = 0;
  return n;
}
static int64_t fetch_response(const char *url, const char *method,
                              const char *body, const char *bearer, int json) {
  CURL *c = curl_easy_init();
  if (!c)
    return 0;
  Buffer b = {0};
  struct curl_slist *headers = NULL;
  headers = curl_slist_append(headers, "Accept: application/json");
  if (bearer && *bearer) {
    size_t n = strlen(bearer) + 24;
    char *h = malloc(n);
    if (!h) {
      curl_slist_free_all(headers);
      curl_easy_cleanup(c);
      return 0;
    }
    snprintf(h, n, "Authorization: Bearer %s", bearer);
    if (!strchr(bearer, '\r') && !strchr(bearer, '\n'))
      headers = curl_slist_append(headers, h);
    free(h);
  }
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 3L);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_USERAGENT, "portfolio-forge/0.1");
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, receive_http);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
  if (json)
    headers = curl_slist_append(headers, "Content-Type: application/json");
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
  if (method && (!strcmp(method, "POST") || !strcmp(method, "PUT") ||
                 !strcmp(method, "DELETE"))) {
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body ? body : "");
  }
  CURLcode rc = curl_easy_perform(c);
  long status = 0;
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
  int64_t out = fw_object();
  fw_set(out, "status", fw_number(rc == CURLE_OK ? status : 0));
  fw_set(out, "data", b.data ? fw_parse(b.data) : 0);
  free(b.data);
  curl_slist_free_all(headers);
  curl_easy_cleanup(c);
  return out;
}
int64_t fw_fetch(const char *url, const char *method, const char *body,
                 const char *bearer) {
  int64_t response = fetch_response(url, method, body, bearer, 0);
  int64_t status = fw_integer(fw_get(response, "status"));
  return status >= 200 && status < 300 ? fw_get(response, "data") : 0;
}
int64_t fw_request(const char *url, const char *method, const char *body,
                   const char *bearer) {
  if (!method || (strcmp(method, "GET") && strcmp(method, "POST") &&
                  strcmp(method, "PUT") && strcmp(method, "DELETE")))
    return 0;
  return fetch_response(url, method, body, bearer, 1);
}
static char *b64(const unsigned char *data, size_t n) {
  char *s = track(malloc(4 * ((n + 2) / 3) + 1), 0);
  if (!s)
    return NULL;
  int len = EVP_EncodeBlock((unsigned char *)s, data, (int)n);
  while (len && s[len - 1] == '=')
    len--;
  s[len] = 0;
  for (int i = 0; i < len; i++) {
    if (s[i] == '+')
      s[i] = '-';
    else if (s[i] == '/')
      s[i] = '_';
  }
  return s;
}
static unsigned char *unb64(const char *s, size_t n, size_t *out) {
  if (!n || n > 65536 || n % 4 == 1)
    return NULL;
  size_t full = ((n + 3) / 4) * 4;
  char *tmp = malloc(full + 1);
  unsigned char *data = track(malloc(full + 1), 0);
  if (!tmp || !data) {
    free(tmp);
    return NULL;
  }
  for (size_t i = 0; i < n; i++) {
    unsigned char ch = s[i];
    if (!(isalnum(ch) || ch == '-' || ch == '_')) {
      free(tmp);
      return NULL;
    }
    tmp[i] = ch == '-' ? '+' : ch == '_' ? '/' : ch;
  }
  for (size_t i = n; i < full; i++)
    tmp[i] = '=';
  tmp[full] = 0;
  int len = EVP_DecodeBlock(data, (unsigned char *)tmp, (int)full);
  free(tmp);
  if (len < 0)
    return NULL;
  *out = (size_t)len - (full - n);
  data[*out] = 0;
  return data;
}
const char *fw_jwt_sign(int64_t claims, const char *secret) {
  if (fw_kind(claims) != 4 || !secret || !*secret)
    return "";
  const char *payload = fw_dump(claims);
  char *p = b64((unsigned char *)payload, strlen(payload));
  if (!p)
    return "";
  size_t n = strlen(p) + 38;
  char *message = track(malloc(n), 0);
  if (!message)
    return "";
  snprintf(message, n, "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.%s", p);
  unsigned char mac[EVP_MAX_MD_SIZE];
  unsigned int len = 0;
  if (!HMAC(EVP_sha256(), secret, (int)strlen(secret), (unsigned char *)message,
            strlen(message), mac, &len))
    return "";
  char *sig = b64(mac, len);
  if (!sig)
    return "";
  size_t total = strlen(message) + strlen(sig) + 2;
  char *token = track(malloc(total), 0);
  if (!token)
    return "";
  snprintf(token, total, "%s.%s", message, sig);
  return token;
}
int64_t fw_jwt_verify(const char *token, const char *secret) {
  if (!token || !secret || !*secret || strlen(token) > 16384)
    return 0;
  const char *a = strchr(token, '.');
  if (!a)
    return 0;
  const char *b = strchr(a + 1, '.');
  if (!b || strchr(b + 1, '.'))
    return 0;
  size_t hn, pn, sn;
  unsigned char *header = unb64(token, a - token, &hn),
                *payload = unb64(a + 1, b - a - 1, &pn),
                *sig = unb64(b + 1, strlen(b + 1), &sn);
  if (!header || !payload || !sig || sn != 32 || strlen((char *)header) != hn ||
      strlen((char *)payload) != pn)
    return 0;
  int64_t h = fw_parse((char *)header);
  if (fw_kind(h) != 4 || strcmp(fw_text(fw_get(h, "alg")), "HS256"))
    return 0;
  unsigned char mac[EVP_MAX_MD_SIZE];
  unsigned int len = 0;
  if (!HMAC(EVP_sha256(), secret, (int)strlen(secret), (unsigned char *)token,
            b - token, mac, &len) ||
      len != sn || CRYPTO_memcmp(mac, sig, sn))
    return 0;
  int64_t claims = fw_parse((char *)payload);
  if (fw_kind(claims) != 4 || fw_kind(fw_get(claims, "exp")) != 2 ||
      fw_integer(fw_get(claims, "exp")) <= fw_now() ||
      fw_kind(fw_get(claims, "sub")) != 3 || !*fw_text(fw_get(claims, "sub")) ||
      fw_kind(fw_get(claims, "role")) != 3)
    return 0;
  return claims;
}
const char *fw_read(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return "";
  if (fseek(f, 0, SEEK_END)) {
    fclose(f);
    return "";
  }
  long size = ftell(f);
  if (size < 0 || size > FETCH_MAX) {
    fclose(f);
    return "";
  }
  rewind(f);
  char *data = track(malloc((size_t)size + 1), 0);
  if (!data) {
    fclose(f);
    return "";
  }
  size_t n = fread(data, 1, size, f);
  fclose(f);
  data[n] = 0;
  return data;
}

typedef struct {
  struct MHD_Connection *connection;
  char *method, *path, *body;
  size_t size;
  struct MHD_Response *response;
  unsigned status;
  char *origin;
  struct MHD_PostProcessor *post;
  int error, fd, files, finalized;
  size_t file_size;
  char temp[512], extension[8];
} Request;
static fw_handler app_handler;
static volatile sig_atomic_t stopping;
const char *fw_method(int64_t h) { return h ? P(Request, h)->method : ""; }
const char *fw_path(int64_t h) { return h ? P(Request, h)->path : ""; }
const char *fw_header(int64_t h, const char *name) {
  const char *s = h ? MHD_lookup_connection_value(P(Request, h)->connection,
                                                  MHD_HEADER_KIND, name)
                    : NULL;
  return s ? s : "";
}
const char *fw_query(int64_t h, const char *name) {
  const char *s = h ? MHD_lookup_connection_value(P(Request, h)->connection,
                                                  MHD_GET_ARGUMENT_KIND, name)
                    : NULL;
  return s ? s : "";
}
const char *fw_body(int64_t h) {
  return h && P(Request, h)->body ? P(Request, h)->body : "";
}
const char *fw_ip(int64_t h) {
  if (!h)
    return "";
  const union MHD_ConnectionInfo *info = MHD_get_connection_info(
      P(Request, h)->connection, MHD_CONNECTION_INFO_CLIENT_ADDRESS);
  if (!info || !info->client_addr)
    return "";
  char ip[INET6_ADDRSTRLEN];
  struct sockaddr *addr = info->client_addr;
  if (addr->sa_family == AF_INET)
    inet_ntop(AF_INET, &((struct sockaddr_in *)addr)->sin_addr, ip, sizeof(ip));
  else if (addr->sa_family == AF_INET6)
    inet_ntop(AF_INET6, &((struct sockaddr_in6 *)addr)->sin6_addr, ip,
              sizeof(ip));
  else
    return "";
  const char *proxy = fw_header(h, "X-Real-IP");
  if (private_ip(ip) && fw_valid(proxy, "ip")) {
    struct in_addr a;
    struct in6_addr b;
    if (inet_pton(AF_INET, proxy, &a) == 1)
      inet_ntop(AF_INET, &a, ip, sizeof(ip));
    else {
      inet_pton(AF_INET6, proxy, &b);
      inet_ntop(AF_INET6, &b, ip, sizeof(ip));
    }
  }
  return copy(ip);
}
const char *fw_segment(int64_t h, int64_t i) { return fw_part(fw_path(h), i); }
int64_t fw_segments(int64_t h) {
  const char *s = fw_path(h);
  if (*s == '/')
    s++;
  if (!*s)
    return 0;
  int n = 1;
  while (*s)
    if (*s++ == '/')
      n++;
  return n;
}
static int64_t response(Request *r, unsigned status, const char *body,
                        const char *type) {
  if (!r)
    return 0;
  if (r->response)
    MHD_destroy_response(r->response);
  r->response = MHD_create_response_from_buffer(body ? strlen(body) : 0,
                                                (void *)(body ? body : ""),
                                                MHD_RESPMEM_MUST_COPY);
  if (!r->response)
    return 0;
  r->status = status;
  MHD_add_response_header(r->response, "Content-Type", type);
  MHD_add_response_header(r->response, "X-Content-Type-Options", "nosniff");
  return 1;
}
int64_t fw_respond(int64_t h, int64_t status, const char *body) {
  return response(P(Request, h), (unsigned)status, body,
                  "application/json; charset=utf-8");
}
int64_t fw_content_type(int64_t h, const char *value) {
  Request *r = P(Request, h);
  if (!r || !r->response)
    return 0;
  MHD_del_response_header(r->response, "Content-Type",
                          "application/json; charset=utf-8");
  return MHD_add_response_header(r->response, "Content-Type", value) == MHD_YES;
}
int64_t fw_redirect(int64_t h, const char *url) {
  if (!h || !url || strchr(url, '\r') || strchr(url, '\n'))
    return 0;
  if (!response(P(Request, h), 302, "", "text/plain"))
    return 0;
  MHD_add_response_header(P(Request, h)->response, "Location", url);
  MHD_add_response_header(P(Request, h)->response, "Cache-Control", "no-store");
  return 1;
}
int64_t fw_cors(int64_t h, const char *allowed) {
  Request *r = P(Request, h);
  if (!r)
    return 0;
  const char *origin = fw_header(h, "Origin");
  if (!*origin || !allowed)
    return 0;
  const char *s = allowed;
  while (*s) {
    const char *e = strchr(s, ',');
    size_t n = e ? (size_t)(e - s) : strlen(s);
    while (n && isspace((unsigned char)*s)) {
      s++;
      n--;
    }
    while (n && isspace((unsigned char)s[n - 1]))
      n--;
    if (n == strlen(origin) && !strncmp(s, origin, n)) {
      free(r->origin);
      r->origin = strdup(origin);
      return 1;
    }
    if (!e)
      break;
    s = e + 1;
  }
  return 0;
}
static const char *mime(const char *name) {
  const char *ext = strrchr(name, '.');
  if (!ext)
    return "application/octet-stream";
  if (!strcasecmp(ext, ".png"))
    return "image/png";
  if (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg"))
    return "image/jpeg";
  if (!strcasecmp(ext, ".gif"))
    return "image/gif";
  if (!strcasecmp(ext, ".webp"))
    return "image/webp";
  if (!strcasecmp(ext, ".svg"))
    return "image/svg+xml";
  if (!strcasecmp(ext, ".pdf"))
    return "application/pdf";
  return "application/octet-stream";
}
int64_t fw_file(int64_t h, const char *root, const char *name) {
  if (!h || !fw_valid(name, "filename"))
    return fw_respond(h, 404, "{\"error\":\"not_found\"}");
  int dir = open(root, O_RDONLY | O_DIRECTORY);
  if (dir < 0)
    return fw_respond(h, 404, "{\"error\":\"not_found\"}");
  int fd = openat(dir, name, O_RDONLY | O_NOFOLLOW);
  close(dir);
  struct stat st;
  if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode)) {
    if (fd >= 0)
      close(fd);
    return fw_respond(h, 404, "{\"error\":\"not_found\"}");
  }
  Request *r = P(Request, h);
  if (r->response)
    MHD_destroy_response(r->response);
  r->response = MHD_create_response_from_fd64((uint64_t)st.st_size, fd);
  if (!r->response) {
    close(fd);
    return 0;
  }
  r->status = 200;
  MHD_add_response_header(r->response, "Content-Type", mime(name));
  MHD_add_response_header(r->response, "X-Content-Type-Options", "nosniff");
  MHD_add_response_header(
      r->response, "Content-Security-Policy",
      "default-src 'none'; style-src 'unsafe-inline'; sandbox");
  return 1;
}
static enum MHD_Result upload_part(void *cls, enum MHD_ValueKind kind,
                                   const char *key, const char *filename,
                                   const char *content_type,
                                   const char *encoding, const char *data,
                                   uint64_t off, size_t size) {
  Request *r = cls;
  (void)kind;
  (void)key;
  (void)content_type;
  (void)encoding;
  if (!filename || r->error)
    return MHD_NO;
  if (off == 0 && r->fd < 0) {
    const char *ext = strrchr(filename, '.');
    if (!ext || strlen(ext) > 7 ||
        !strcmp(mime(ext), "application/octet-stream")) {
      r->error = 400;
      return MHD_NO;
    }
    if (++r->files > 1) {
      r->error = 400;
      return MHD_NO;
    }
    snprintf(r->extension, sizeof(r->extension), "%s", ext);
    for (char *p = r->extension; *p; p++)
      *p = (char)tolower((unsigned char)*p);
    const char *root = fw_env("UPLOAD_DIR", "./uploads");
    if (mkdir(root, 0755) && errno != EEXIST) {
      r->error = 500;
      return MHD_NO;
    }
    if (snprintf(r->temp, sizeof(r->temp), "%s/.upload-XXXXXX", root) >=
        (int)sizeof(r->temp)) {
      r->error = 500;
      return MHD_NO;
    }
    r->fd = mkstemp(r->temp);
    if (r->fd < 0) {
      r->error = 500;
      return MHD_NO;
    }
  } else if (off == 0 && r->fd >= 0 && r->file_size) {
    r->error = 400;
    return MHD_NO;
  }
  if (off != r->file_size || size > FILE_MAX - r->file_size) {
    r->error = 413;
    return MHD_NO;
  }
  size_t written = 0;
  while (written < size) {
    ssize_t n = write(r->fd, data + written, size - written);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0) {
      r->error = 500;
      return MHD_NO;
    }
    written += (size_t)n;
  }
  r->file_size += size;
  return MHD_YES;
}
const char *fw_upload(int64_t h, const char *root) {
  Request *r = P(Request, h);
  if (!r || r->error || r->fd < 0 || r->files != 1 || !r->file_size ||
      r->finalized)
    return "";
  char destination[512], name[64];
  snprintf(name, sizeof(name), "%s%s", fw_uuid(), r->extension);
  if (!*name || snprintf(destination, sizeof(destination), "%s/%s", root,
                         name) >= (int)sizeof(destination))
    return "";
  if (fsync(r->fd) || fchmod(r->fd, 0644) || rename(r->temp, destination))
    return "";
  close(r->fd);
  r->fd = -1;
  r->finalized = 1;
  r->temp[0] = 0;
  return copy(name);
}
static void completed(void *cls, struct MHD_Connection *connection,
                      void **context, enum MHD_RequestTerminationCode code) {
  (void)cls;
  (void)connection;
  (void)code;
  Request *r = *context;
  if (!r)
    return;
  if (r->post)
    MHD_destroy_post_processor(r->post);
  if (r->fd >= 0)
    close(r->fd);
  if (*r->temp)
    unlink(r->temp);
  if (r->response)
    MHD_destroy_response(r->response);
  free(r->method);
  free(r->path);
  free(r->body);
  free(r->origin);
  free(r);
  *context = NULL;
}
static enum MHD_Result serve(void *cls, struct MHD_Connection *connection,
                             const char *path, const char *method,
                             const char *version, const char *data,
                             size_t *size, void **context) {
  (void)cls;
  (void)version;
  Request *r = *context;
  if (!r) {
    r = calloc(1, sizeof(*r));
    if (!r)
      return MHD_NO;
    r->fd = -1;
    r->connection = connection;
    r->method = strdup(method);
    r->path = strdup(path);
    *context = r;
    if (!r->method || !r->path)
      return MHD_NO;
    const char *type = MHD_lookup_connection_value(connection, MHD_HEADER_KIND,
                                                   "Content-Type");
    if (type && !strncasecmp(type, "multipart/form-data", 19)) {
      r->post = MHD_create_post_processor(connection, 65536, upload_part, r);
      if (!r->post)
        r->error = 400;
    }
    return MHD_YES;
  }
  if (*size) {
    if (*size > BODY_MAX - r->size)
      r->error = 413;
    if (!r->post && memchr(data, 0, *size))
      r->error = 400;
    if (!r->error) {
      if (r->post) {
        if (MHD_post_process(r->post, data, *size) != MHD_YES && !r->error)
          r->error = 400;
      } else if (*size > JSON_MAX - r->size)
        r->error = 413;
      else {
        char *buf = realloc(r->body, r->size + *size + 1);
        if (!buf)
          r->error = 500;
        else {
          r->body = buf;
          memcpy(buf + r->size, data, *size);
          buf[r->size + *size] = 0;
        }
      }
      r->size += *size;
    }
    *size = 0;
    return MHD_YES;
  }
  if (r->response)
    return MHD_YES;
  fw_scope_begin();
  if (r->error)
    fw_respond(
        H(r), r->error,
        r->error == 413
            ? "{\"error\":\"validation\",\"message\":\"payload too large\"}"
        : r->error == 400
            ? "{\"error\":\"validation\",\"message\":\"invalid upload\"}"
            : "{\"error\":\"internal\"}");
  else
    app_handler(H(r));
  if (!r->response)
    fw_respond(H(r), 500, "{\"error\":\"internal\"}");
  if (r->origin) {
    MHD_add_response_header(r->response, "Access-Control-Allow-Origin",
                            r->origin);
    MHD_add_response_header(r->response, "Vary", "Origin");
    MHD_add_response_header(r->response, "Access-Control-Allow-Methods",
                            "GET, POST, PUT, DELETE, OPTIONS");
    MHD_add_response_header(r->response, "Access-Control-Allow-Headers",
                            "authorization, content-type");
    MHD_add_response_header(r->response, "Access-Control-Max-Age", "3600");
  }
  enum MHD_Result result =
      MHD_queue_response(connection, r->status, r->response);
  fw_scope_end();
  return result;
}
static void stop_server(int sig) {
  (void)sig;
  stopping = 1;
}
int64_t fw_run(const char *host, int64_t port, int64_t workers,
               fw_handler handler) {
  if (!host || !handler || port < 1 || port > 65535 || workers < 1 ||
      workers > 64)
    return 1;
  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, host, &addr.sin_addr) != 1)
    return 1;
  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
    return 1;
  app_handler = handler;
  stopping = 0;
  signal(SIGTERM, stop_server);
  signal(SIGINT, stop_server);
  signal(SIGPIPE, SIG_IGN);
  struct MHD_Daemon *d = MHD_start_daemon(
      MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_ERROR_LOG, (uint16_t)port, NULL,
      NULL, serve, NULL, MHD_OPTION_SOCK_ADDR, &addr,
      MHD_OPTION_THREAD_POOL_SIZE, (unsigned int)workers,
      MHD_OPTION_CONNECTION_LIMIT, 256u, MHD_OPTION_CONNECTION_TIMEOUT, 30u,
      MHD_OPTION_CONNECTION_MEMORY_LIMIT, (size_t)128 * 1024,
      MHD_OPTION_NOTIFY_COMPLETED, completed, NULL, MHD_OPTION_END);
  if (!d) {
    curl_global_cleanup();
    return 1;
  }
  fprintf(stderr, "Forge portfolio API listening on %s:%lld\n", host,
          (long long)port);
  while (!stopping) {
    struct timespec delay = {0, 100000000};
    nanosleep(&delay, NULL);
  }
  MHD_stop_daemon(d);
  curl_global_cleanup();
  return 0;
}

const char *fw_normalize_ip(const char *ip) {
  struct in_addr a;
  struct in6_addr b;
  char result[INET6_ADDRSTRLEN];
  if (inet_pton(AF_INET, ip, &a) == 1) {
    inet_ntop(AF_INET, &a, result, sizeof(result));
    return copy(result);
  }
  if (inet_pton(AF_INET6, ip, &b) == 1) {
    if (IN6_IS_ADDR_V4MAPPED(&b))
      inet_ntop(AF_INET, &b.s6_addr[12], result, sizeof(result));
    else
      inet_ntop(AF_INET6, &b, result, sizeof(result));
    return copy(result);
  }
  return "";
}
int64_t fw_sort(int64_t array, const char *key) {
  if (fw_kind(array) != 5)
    return 0;
  size_t count = (size_t)fw_count(array);
  for (size_t i = 1; i < count; i++) {
    size_t j = i;
    while (j > 0) {
      struct json_object *a =
          json_object_array_get_idx(P(struct json_object, array), j - 1);
      struct json_object *b =
          json_object_array_get_idx(P(struct json_object, array), j);
      if (strcasecmp(fw_text(fw_get(H(a), key)), fw_text(fw_get(H(b), key))) <=
          0)
        break;
      json_object_get(a);
      json_object_get(b);
      json_object_array_put_idx(P(struct json_object, array), j - 1, b);
      json_object_array_put_idx(P(struct json_object, array), j, a);
      j--;
    }
  }
  return 1;
}
typedef struct {
  int64_t urls;
  const char *bearer;
  char **results;
  size_t next, count;
  pthread_mutex_t mutex;
} FetchBatch;
static void *fetch_worker(void *arg) {
  FetchBatch *batch = arg;
  for (;;) {
    pthread_mutex_lock(&batch->mutex);
    size_t i = batch->next++;
    pthread_mutex_unlock(&batch->mutex);
    if (i >= batch->count)
      break;
    fw_scope_begin();
    int64_t value = fw_fetch(fw_text(fw_at(batch->urls, (int64_t)i)), "GET", "",
                             batch->bearer);
    batch->results[i] = strdup(fw_dump(value));
    fw_scope_end();
  }
  return NULL;
}
int64_t fw_fetch_many(int64_t urls, const char *bearer) {
  int64_t out = fw_array();
  size_t count = (size_t)fw_count(urls);
  if (!count || count > 300)
    return out;
  char **results = calloc(count, sizeof(char *));
  if (!results)
    return out;
  FetchBatch batch = {
      .urls = urls, .bearer = bearer, .results = results, .count = count};
  pthread_mutex_init(&batch.mutex, NULL);
  pthread_t threads[4];
  size_t started = 0;
  for (size_t i = 0; i < 4; i++) {
    if (pthread_create(&threads[started], NULL, fetch_worker, &batch) == 0)
      started++;
  }
  if (!started) {
    for (size_t i = 0; i < count; i++)
      fw_push(out,
              fw_fetch(fw_text(fw_at(urls, (int64_t)i)), "GET", "", bearer));
  } else {
    for (size_t i = 0; i < started; i++)
      pthread_join(threads[i], NULL);
    for (size_t i = 0; i < count; i++) {
      fw_push(out, fw_parse(results[i]));
      free(results[i]);
    }
  }
  pthread_mutex_destroy(&batch.mutex);
  free(results);
  return out;
}

int64_t fw_has(int64_t h, const char *key) {
  struct json_object *value = NULL;
  return fw_kind(h) == 4 &&
         json_object_object_get_ex(P(struct json_object, h), key, &value);
}
const char *fw_cookie(int64_t request, const char *name) {
  const char *s = fw_header(request, "Cookie");
  size_t length = strlen(name);
  while (*s) {
    while (*s == ' ' || *s == ';')
      s++;
    const char *end = strchr(s, ';');
    size_t count = end ? (size_t)(end - s) : strlen(s);
    if (count > length && !strncmp(s, name, length) && s[length] == '=')
      return track(strndup(s + length + 1, count - length - 1), 0);
    if (!end)
      break;
    s = end + 1;
  }
  return "";
}
int64_t fw_set_cookie(int64_t request, const char *name, const char *value,
                      int64_t age, int64_t secure) {
  Request *r = P(Request, request);
  if (!r || !r->response || age < 0 || age > 86400)
    return 0;
  for (const char *p = name; *p; p++)
    if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '-'))
      return 0;
  for (const char *p = value; *p; p++)
    if (!(isalnum((unsigned char)*p) || strchr("_-.", *p)))
      return 0;
  char buffer[1024];
  if (snprintf(buffer, sizeof(buffer),
               "%s=%s; Max-Age=%lld; Path=/api/auth/github; HttpOnly; "
               "SameSite=Lax%s",
               name, value, (long long)age,
               secure ? "; Secure" : "") >= (int)sizeof(buffer))
    return 0;
  return MHD_add_response_header(r->response, "Set-Cookie", buffer) == MHD_YES;
}
