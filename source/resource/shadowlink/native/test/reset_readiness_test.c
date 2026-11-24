#include "runtime.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *health;
static long http_status = 200;
static int transported = 1, writes, delivered = 1;
int __wrap_sl_http(const char *url, const char *body, const char *authorization,
                  unsigned timeout, char **response, long *status)
{
    assert(!strcmp(url, "http://127.0.0.1:9002/kiosk-health"));
    assert(!body && !authorization && timeout == 2);
    *response = g_strdup(health); *status = http_status; return transported;
}
int __wrap_sl_websocket_send(const char *url, const char *json, double timeout)
{
    /* Readiness and RESET must use the same renderer connection path. */
    assert(!strcmp(url, "ws://127.0.0.1:9002/ws"));
    assert(!strcmp(json, "{\"event\":\"RESET\"}") && timeout == 5);
    writes++; return delivered;
}
int main(void)
{
    const char *unready[] = {NULL, "ok", "{}",
        "{\"connected\":false,\"healthy\":false,\"ackAgeMs\":null}",
        "{\"connected\":true,\"healthy\":false,\"ackAgeMs\":null}",
        "{\"connected\":false,\"healthy\":true,\"ackAgeMs\":1}",
        "{\"connected\":true,\"healthy\":true,\"ackAgeMs\":15000}",
        "{\"connected\":true,\"healthy\":\"true\",\"ackAgeMs\":1}"};
    for (size_t i = 0; i < G_N_ELEMENTS(unready); ++i) {
        health = unready[i]; assert(!sl_notify_unbind_reset()); assert(writes == 0);
    }
    /* performance.now() produces fractional milliseconds on the real kiosk.
     * Reject coercible/non-finite values and preserve the exact age boundary. */
    const char *invalid_ages[] = {"null", "true", "\"1\"", "[]", "{}",
        "-1", "-0.1", "15000.0", "15000.1", "1e100", "NaN", "Infinity", "-Infinity"};
    for (size_t i = 0; i < G_N_ELEMENTS(invalid_ages); ++i) {
        char *response = g_strdup_printf("{\"connected\":true,\"healthy\":true,\"ackAgeMs\":%s}", invalid_ages[i]);
        health = response;
        assert(!sl_notify_unbind_reset()); assert(writes == 0);
        g_free(response);
    }
    const char *ready_ages[] = {"0", "1", "14999", "0.0", "0.1",
        "1901.6719080023468", "14999.999", "1.9e3"};
    for (size_t i = 0; i < G_N_ELEMENTS(ready_ages); ++i) {
        char *response = g_strdup_printf("{\"connected\":true,\"healthy\":true,\"ackAgeMs\":%s}", ready_ages[i]);
        health = response;
        assert(sl_notify_unbind_reset()); assert(writes == (int)i + 1);
        g_free(response);
    }
    writes = 0;
    health = "{\"connected\":true,\"healthy\":true,\"ackAgeMs\":1}";
    transported = 0; assert(!sl_notify_unbind_reset());
    transported = 1; http_status = 503; assert(!sl_notify_unbind_reset());
    assert(writes == 0);
    http_status = 200; delivered = 0;
    assert(!sl_notify_unbind_reset() && writes == 1);
    delivered = 1; assert(sl_notify_unbind_reset() && writes == 2);
    puts("PASS reset waits for renderer readiness and propagates send failure");
    return 0;
}
