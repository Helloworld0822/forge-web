#include "forge_web.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
static int64_t handler(int64_t request) {
  if (!strcmp(fw_path(request), "/slow")) {
    fprintf(stderr, "SLOW_STARTED\n");
    struct timespec delay = {1, 0};
    while (nanosleep(&delay, &delay) && errno == EINTR) {}
  }
  const char *ip = fw_ip(request);
  assert(ip == fw_ip(request));
  int64_t result = fw_object();
  fw_set(result, "bytes", fw_number((int64_t)strlen(fw_body(request))));
  fw_set(result, "ip", fw_string(ip));
  fw_set(result, "path", fw_string(fw_path(request)));
  fw_set(result, "count", fw_number(fw_segments(request)));
  int64_t parts = fw_array();
  for (int64_t i = 0; i < fw_segments(request) && i < 10; i++)
    fw_push(parts, fw_string(fw_segment(request, i)));
  fw_set(result, "parts", parts);
  return fw_respond(request, 200, fw_dump(result));
}
int main(int argc, char **argv) {
  return argc == 2 || argc == 3
      ? (int)fw_run("127.0.0.1", atoi(argv[1]), argc == 3 ? atoi(argv[2]) : 2, handler)
      : 1;
}
