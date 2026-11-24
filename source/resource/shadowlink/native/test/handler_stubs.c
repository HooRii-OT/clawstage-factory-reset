/* Linked only into the parity-test executable, never into the runtime bundle. */
#include "device.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static int setting(const char *name, int fallback)
{
    const char *value = getenv(name); return value ? atoi(value) : fallback;
}
guint __real_g_timeout_add_seconds(guint interval, GSourceFunc function, gpointer data);
guint __wrap_g_timeout_add_seconds(guint interval, GSourceFunc function, gpointer data)
{
    if (interval == 30) interval = (guint)setting("SL_TEST_USERINFO_TIMEOUT", 30);
    return __real_g_timeout_add_seconds(interval, function, data);
}
int __wrap_sl_volume(int percent) { return percent < 0 ? setting("SL_TEST_VOLUME", 64) : percent == 13 ? -1 : percent; }
int __wrap_sl_backlight(int percent) { return percent < 0 ? setting("SL_TEST_BACKLIGHT", 75) : percent == 13 ? -1 : percent; }
int __wrap_sl_wifi_connect(const char *ssid, const char *password, uint8_t ip[4])
{
    (void)ssid; (void)password;
    g_usleep((gulong)setting("SL_TEST_WIFI_DELAY_MS", 0) * 1000);
    const uint8_t address[] = {192, 0, 2, 10}; memcpy(ip, address, 4); return setting("SL_TEST_WIFI_RESULT", 1);
}
char *__wrap_sl_wifi_ssid(void) { return g_strdup(""); }
int __wrap_sl_wifi_ip(uint8_t ip[4]) { (void)ip; return 0; }
void __wrap_sl_storage(uint64_t usage[4]) { const uint64_t sample[] = {10, 100, 20, 200}; memcpy(usage, sample, sizeof(sample)); }
int __wrap_sl_run(const char *const argv[], unsigned timeout, char **output)
{
    (void)timeout; if (output) *output = g_strdup("");
    const char *trace = getenv("SL_TEST_UNBIND_TRACE");
    if (trace && (!strcmp(argv[0], "runuser") || !strcmp(argv[0], "env"))) {
        FILE *file = fopen(trace, "a");
        if (file) { fprintf(file, "%s\n", !strcmp(argv[0], "runuser") ? "clear" : "purge"); fclose(file); }
    }
    return strcmp(argv[0], "systemctl") != 0;
}
int __wrap_sl_http(const char *url, const char *body, const char *authorization, unsigned timeout, char **response, long *status)
{
    (void)authorization; (void)timeout;
    if (body && strstr(url, "/hoorii/device/unbind")) {
        const char *trace = getenv("SL_TEST_UNBIND_TRACE");
        if (trace) { FILE *file = fopen(trace, "a"); if (file) { fputs("cloud\n", file); fclose(file); } }
        const char *failure = getenv("SL_TEST_UNBIND_FAILURE");
        if (failure) {
            *status = atoi(failure);
            *response = g_strdup("{\"success\":false,\"allowReset\":false}");
            return *status != 0;
        }
    }
    const char *hub = getenv("SL_TEST_UNBIND_HUB");
    if (hub && body && strstr(url, "/hoorii/device/unbind")) {
        json_object *request = sl_json_parse(body, strlen(body));
        *response = g_strdup_printf("{\"success\":true,\"data\":{\"requestId\":\"%s\",\"action\":\"unbind\",\"hubId\":\"%s\",\"allowReset\":true}}",
            sl_string(request, "requestId"), hub);
        *status = !strcmp(sl_string(request, "action"), "unbind") ? 200 : 400;
        json_object_put(request); return 1;
    }
    if (body && strstr(url, "/hoorii/byok/profile")) {
        const char *reply = getenv("SL_TEST_HTTP_BODY");
        if (response) *response = g_strdup(reply ? reply : "{\"success\":true}");
        *status = setting("SL_TEST_HTTP_STATUS", 200); return 1;
    }
    if (response) *response = NULL;
    *status = 503; return 0;
}
gboolean __wrap_sl_reset_in_progress(void) { return FALSE; }
int __wrap_sl_reset_services(sl_runtime *r, gboolean from_shadowlink, gboolean *accepted)
{
    (void)r; (void)from_shadowlink;
    const char *trace = getenv("SL_TEST_RESET_TRACE");
    if (trace) { FILE *file = fopen(trace, "a"); if (file) { fputs("reset\n", file); fclose(file); } }
    if (accepted) *accepted = TRUE;
    return 1;
}
int __wrap_sl_websocket_event(const char *json)
{
    const char *trace = getenv("SL_TEST_WS_TRACE");
    if (trace) { FILE *file = fopen(trace, "a"); if (file) { fprintf(file, "%s\n", json); fclose(file); } }
    return 1;
}

int __wrap_sl_notify_unbind_reset(void) { return __wrap_sl_websocket_event("{\"event\":\"RESET\"}"); }
