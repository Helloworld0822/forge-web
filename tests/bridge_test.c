#include "forge_web.h"
#include <assert.h>
#include <string.h>
int main(void) {
  for (int i = 0; i < 1000; i++) {
    fw_scope_begin();
    int64_t a = fw_parse("{\"integer\":9007199254740993,\"a\":[],\"b\":false}");
    assert(fw_integer(fw_get(a, "integer")) == INT64_C(9007199254740993));
    assert(fw_kind(fw_get(a, "b")) == 1);
    assert(fw_parse("{\"a\":\"\xff\"}") == 0);
    assert(fw_parse("[] trailing") == 0);
    assert(fw_parse("true") && fw_kind(fw_parse("true")) == 1);
    assert(fw_valid("::ffff:10.0.0.1", "public_ip") == 0);
    assert(!strcmp(fw_normalize_ip("::ffff:8.8.8.8"), "8.8.8.8"));
    int64_t b = fw_array();
    fw_push(b, a);
    fw_set(a, "x", fw_string("hello"));
    assert(strstr(fw_dump(b), "hello"));
    fw_scope_end();
  }
  return 0;
}
