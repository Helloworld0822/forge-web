#include "forge_web.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
  assert(argc == 2);
  char url[512];
  const char *methods[] = {"POST", "GET", "PUT", "DELETE", "GET"};
  for (int i = 0; i < 5; i++) {
    fw_scope_begin();
    snprintf(url, sizeof(url), "%s/echo", argv[1]);
    int64_t r = fw_request(url, methods[i], "{\"value\":1}",
                           i == 0 ? "private-token" : "");
    assert(fw_integer(fw_get(r, "status")) == 200);
    int64_t d = fw_get(r, "data");
    assert(!strcmp(fw_text(fw_get(d, "method")), methods[i]));
    assert(!strcmp(fw_text(fw_get(d, "authorization")),
                   i == 0 ? "Bearer private-token" : ""));
    assert(!strcmp(fw_text(fw_get(d, "body")),
                   i == 1 || i == 4 ? "" : "{\"value\":1}"));
    assert(fw_integer(fw_get(d, "connection")) == 1);
    fw_scope_end();
  }
  fw_scope_begin();
  snprintf(url, sizeof(url), "%s/oversized", argv[1]);
  int64_t r = fw_request(url, "GET", "", "");
  assert(fw_integer(fw_get(r, "status")) == 0);
  snprintf(url, sizeof(url), "%s/redirect", argv[1]);
  r = fw_request(url, "GET", "", "secret");
  assert(fw_integer(fw_get(r, "status")) == 302);
  snprintf(url, sizeof(url), "%s/echo", argv[1]);
  r = fw_request(url, "GET", "", "");
  assert(fw_integer(fw_get(r, "status")) == 200);
  assert(!strcmp(fw_text(fw_get(fw_get(r, "data"), "authorization")), ""));
  r = fw_request("file:///etc/passwd", "GET", "", "");
  assert(fw_integer(fw_get(r, "status")) == 0);
  fw_scope_end();
  return 0;
}
