#include "forge_web.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int64_t handler(int64_t request) {
  char result[80];
  snprintf(result, sizeof(result), "{\"bytes\":%zu}", strlen(fw_body(request)));
  return fw_respond(request, 200, result);
}
int main(int argc, char **argv) {
  return argc == 2 ? (int)fw_run("127.0.0.1", atoi(argv[1]), 2, handler) : 1;
}
