#ifndef FORGE_WEB_H
#define FORGE_WEB_H
#include <stdint.h>
int64_t fw_scope_begin(void);
int64_t fw_scope_end(void);
const char *fw_env(const char *name, const char *fallback);
int64_t fw_parse(const char *text);
int64_t fw_kind(int64_t value);
int64_t fw_get(int64_t value, const char *key);
int64_t fw_at(int64_t value, int64_t index);
int64_t fw_count(int64_t value);
const char *fw_text(int64_t value);
int64_t fw_integer(int64_t value);
int64_t fw_boolean(int64_t value);
const char *fw_dump(int64_t value);
int64_t fw_object(void);
int64_t fw_array(void);
int64_t fw_string(const char *value);
int64_t fw_number(int64_t value);
int64_t fw_bool(int64_t value);
int64_t fw_set(int64_t value, const char *key, int64_t child);
int64_t fw_push(int64_t value, int64_t child);
int64_t fw_keys(int64_t value);
const char *fw_method(int64_t request);
const char *fw_path(int64_t request);
const char *fw_header(int64_t request, const char *name);
const char *fw_query(int64_t request, const char *name);
const char *fw_body(int64_t request);
const char *fw_ip(int64_t request);
const char *fw_segment(int64_t request, int64_t index);
int64_t fw_segments(int64_t request);
int64_t fw_respond(int64_t request, int64_t status, const char *body);
int64_t fw_redirect(int64_t request, const char *url);
int64_t fw_file(int64_t request, const char *root, const char *name);
const char *fw_upload(int64_t request, const char *root);
int64_t fw_cors(int64_t request, const char *origins);
int64_t fw_content_type(int64_t request, const char *value);
const char *fw_trim(const char *value);
int64_t fw_chars(const char *value);
int64_t fw_valid(const char *value, const char *kind);
const char *fw_lower(const char *value);
int64_t fw_equal(const char *a, const char *b);
int64_t fw_starts(const char *value, const char *prefix);
const char *fw_part(const char *value, int64_t index);
const char *fw_uuid(void);
int64_t fw_now(void);
int64_t fw_rate(const char *key, int64_t maximum, int64_t seconds);
const char *fw_urlencode(const char *value);
int64_t fw_fetch(const char *url, const char *method, const char *body,
                 const char *bearer);
int64_t fw_jwt_verify(const char *token, const char *secret);
const char *fw_jwt_sign(int64_t claims, const char *secret);
const char *fw_read(const char *path);
typedef int64_t (*fw_handler)(int64_t);
int64_t fw_run(const char *host, int64_t port, int64_t workers,
               fw_handler handler);
const char *fw_normalize_ip(const char *value);
int64_t fw_sort(int64_t array, const char *key);
int64_t fw_fetch_many(int64_t urls, const char *bearer);
int64_t fw_has(int64_t value, const char *key);
const char *fw_cookie(int64_t request, const char *name);
int64_t fw_set_cookie(int64_t request, const char *name, const char *value,
                      int64_t age, int64_t secure);
#endif
