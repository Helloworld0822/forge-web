/* Compile this unchanged harness against baseline or optimized bridge source.
 */
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
int main(int argc, char **argv) {
  if (argc == 2) {
    double start = seconds();
    for (int i = 0; i < 300; i++) {
      fw_scope_begin();
      int64_t r = fw_request(argv[1], "GET", "", "");
      assert(fw_integer(fw_get(r, "status")) == 200);
      fw_scope_end();
    }
    printf("{\"http_seconds\":%.9f,\"http_requests\":300}\n",
           seconds() - start);
    return 0;
  }
  double start = seconds();
  for (unsigned pass = 0; pass < 40; pass++) {
    Buffer b = {0};
    char chunk[37];
    memset(chunk, 'x', sizeof(chunk));
    for (size_t n = 0; n < FETCH_MAX;) {
      size_t size =
          FETCH_MAX - n < sizeof(chunk) ? FETCH_MAX - n : sizeof(chunk);
      assert(receive_http(chunk, 1, size, &b) == size);
      n += size;
    }
    free(b.data);
  }
  double buffer = seconds() - start;
  start = seconds();
  for (int pass = 0; pass < 500; pass++) {
    fw_scope_begin();
    for (int i = 0; i < 2000; i++)
      assert(fw_number(i));
    fw_scope_end();
  }
  double ownership = seconds() - start;
  char keys[2048][30];
  for (int i = 0; i < 2048; i++) {
    snprintf(keys[i], sizeof(keys[i]), "benchmark-client-%d", i);
    assert(fw_rate(keys[i], INT64_MAX, 3600));
  }
  start = seconds();
  for (int i = 0; i < 200000; i++)
    assert(fw_rate(keys[i & 2047], INT64_MAX, 3600));
  double rate = seconds() - start;
  printf("{\"buffer_seconds\":%.9f,\"ownership_seconds\":%.9f,\"rate_seconds\":"
         "%.9f,\"buffer_bytes\":167772160,\"owned_values\":1000000,\"rate_"
         "checks\":200000}\n",
         buffer, ownership, rate);
  return 0;
}
