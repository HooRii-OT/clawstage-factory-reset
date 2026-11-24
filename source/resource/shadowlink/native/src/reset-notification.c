#include "runtime.h"
#include <math.h>
#include <string.h>

/* A listening relay is not a ready renderer. Use StageUI's existing renderer
 * health contract, then deliver on that same (software) relay. Both local WS
 * inputs already support RESET. The caller retains durable intent because a
 * successful write still does not acknowledge completed browser cleanup. */
int sl_notify_unbind_reset(void)
{
    char *response = NULL;
    long status = 0;
    int transported = sl_http("http://127.0.0.1:9002/kiosk-health", NULL, NULL, 2, &response, &status);
    json_object *health = response ? sl_json_parse(response, strlen(response)) : NULL;
    json_object *healthy = NULL, *connected = NULL, *age = NULL;
    /* StageUI uses performance.now(): elapsed milliseconds may be fractional.
     * Do not coerce strings/bools or truncate negative fractions to zero. */
    int numeric_age = health && json_object_object_get_ex(health, "ackAgeMs", &age) &&
        (json_object_is_type(age, json_type_int) || json_object_is_type(age, json_type_double));
    double age_ms = numeric_age ? json_object_get_double(age) : NAN;
    int ready = transported && status == 200 && health &&
        json_object_object_get_ex(health, "healthy", &healthy) &&
        json_object_is_type(healthy, json_type_boolean) && json_object_get_boolean(healthy) &&
        json_object_object_get_ex(health, "connected", &connected) &&
        json_object_is_type(connected, json_type_boolean) && json_object_get_boolean(connected) &&
        isfinite(age_ms) && age_ms >= 0 && age_ms < 15000;
    json_object_put(health); g_free(response);
    if (!ready) {
        g_print("Shadowlink reset: renderer not ready; retaining notification for retry\n");
        return 0;
    }
    return sl_websocket_send("ws://127.0.0.1:9002/ws", "{\"event\":\"RESET\"}", 5);
}
