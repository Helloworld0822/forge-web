#include <assert.h>
#include <stdio.h>
#ifndef WEB_SOURCE
#define WEB_SOURCE "../src/bridge.c"
#endif
#include WEB_SOURCE
static double seconds(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return now.tv_sec + now.tv_nsec / 1e9;
}
int main(void) {
  fw_scope_begin();
  const char *examples[] = {"{\"role\":\"admin\\u0000user\"}",
                            "{\"role\":\"user\",\"role\":\"admin\"}",
                            "{\"r\\u006fle\":1,\"role\":2}",
                            "9223372036854775808", "NaN"};
  printf("{\"accepted_before_fixes\":[");
  for (unsigned i = 0; i < sizeof(examples) / sizeof(examples[0]); i++)
    printf("%s%d", i ? "," : "", fw_parse(examples[i]) != 0);
  printf("],");
  int64_t claims = fw_object();
  fw_set(claims, "sub", fw_string("benchmark"));
  fw_set(claims, "role", fw_string("user"));
  fw_set(claims, "exp", fw_number(fw_now() + 3600));
  char body[9001];
  memset(body, 'a', sizeof(body) - 1);
  body[sizeof(body) - 1] = 0;
  fw_set(claims, "padding", fw_string(body));
  char *token = strdup(fw_jwt_sign(claims, "incorrect-key"));
  assert(token && *token);
  fw_scope_end();
  double start = seconds();
  for (int i = 0; i < 20000; i++) {
    fw_scope_begin();
    assert(!fw_jwt_verify(token, "correct-key"));
    fw_scope_end();
  }
  printf("\"invalid_jwt_checks\":20000,\"invalid_jwt_seconds\":%.9f}\n",
         seconds() - start);
  free(token);
  return 0;
}
