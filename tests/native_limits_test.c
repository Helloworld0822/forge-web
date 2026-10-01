/* Include the bridge to exercise chunk callbacks and bounded storage directly,
 * alongside the public API; no production test hooks or exported ABI changes.
 */
#include "../src/bridge.c"
#include <assert.h>
#include <limits.h>
#include <stdatomic.h>

static atomic_int successes;
static void *rate_worker(void *unused) {
  (void)unused;
  for (int i = 0; i < 10000; i++)
    atomic_fetch_add(&successes,
                     (int)fw_rate("concurrent-shared", 12345, 3600));
  return NULL;
}
static void *scope_worker(void *unused) {
  (void)unused;
  for (int pass = 0; pass < 40; pass++) {
    fw_scope_begin();
    int64_t a = fw_array();
    for (int j = 0; j < 1500; j++) {
      assert(fw_push(a, fw_string("a reference-counted child")));
      assert(fw_trim("  temporary text  "));
    }
    assert(fw_count(a) == 1500);
    fw_scope_end();
    assert(thread_state->current == &thread_state->first);
    assert(thread_state->first.used == 0);
    assert(thread_state->cached <= 15);
  }
  /* Deliberately leave an open scope: the thread destructor must free it. */
  assert(fw_parse("{\"open\":true}"));
  for (int j = 0; j < 500; j++)
    assert(fw_string("thread exits before explicit scope_end"));
  return NULL;
}
static void test_buffers(void) {
  Buffer b = {0};
  char data[37];
  memset(data, 'x', sizeof(data));
  for (size_t n = 0; n < FETCH_MAX;) {
    size_t chunk = FETCH_MAX - n < sizeof(data) ? FETCH_MAX - n : sizeof(data);
    assert(receive_http(data, 1, chunk, &b) == chunk);
    n += chunk;
    assert(b.size == n && b.data[n] == 0 && b.capacity <= FETCH_MAX + 1);
  }
  assert(receive_http(data, 1, 1, &b) == 0);
  assert(b.size == FETCH_MAX && b.data[b.size] == 0);
  assert(receive_http(data, SIZE_MAX, 2, &b) == 0);
  free(b.data);
  char *json = NULL;
  size_t capacity = 0;
  assert(reserve_buffer(&json, &capacity, JSON_MAX + 1, JSON_MAX));
  assert(capacity == JSON_MAX + 1);
  assert(!reserve_buffer(&json, &capacity, JSON_MAX + 2, JSON_MAX));
  free(json);
}
int main(void) {
  test_buffers();
  assert(!fw_rate(NULL, 1, 1));
  assert(!fw_rate("invalid", 0, 1));
  assert(fw_rate("empty-window", 2, 3600));
  assert(fw_rate("empty-window", 2, 3600));
  assert(!fw_rate("empty-window", 2, 3600));
  pthread_t threads[8];
  for (int i = 0; i < 8; i++)
    assert(!pthread_create(&threads[i], NULL, rate_worker, NULL));
  for (int i = 0; i < 8; i++)
    assert(!pthread_join(threads[i], NULL));
  assert(atomic_load(&successes) == 12345);
  /* Fill the table, then force the oldest slot to exercise hash unlinking. */
  while (rate_used < RATE_CAPACITY) {
    char key[40];
    snprintf(key, sizeof(key), "capacity-%u", rate_used);
    assert(fw_rate(key, 1, 3600));
  }
  for (unsigned i = 0; i < RATE_CAPACITY; i++)
    rates[i].start = time(NULL) + 10;
  assert(!fw_rate("same-second-overflow", 1, 3600));
  rates[10].start -= 100;
  char evicted[256];
  snprintf(evicted, sizeof(evicted), "%s", rates[10].key);
  assert(fw_rate("oldest-replacement", 1, 3600));
  assert(!fw_rate("oldest-replacement", 1, 3600));
  assert(!fw_rate(evicted, 1, 3600));
  rates[10].start = time(NULL) - 3601;
  assert(fw_rate("oldest-replacement", 1, 3600));
  for (int i = 0; i < 8; i++)
    assert(!pthread_create(&threads[i], NULL, scope_worker, NULL));
  for (int i = 0; i < 8; i++)
    assert(!pthread_join(threads[i], NULL));
  return 0;
}
