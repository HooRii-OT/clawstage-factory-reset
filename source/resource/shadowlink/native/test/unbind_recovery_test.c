#define _GNU_SOURCE
#include "device.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* Real journal/config writes and abrupt process exits; every external service
 * and cleanup command is replaced. Nothing touches device data or services. */
static sl_runtime runtime;
static char *root, *journal_path, *trace_path;
static const char *cut;
static int gateway_ready = 1, cloud_ready = 1, ws_ready = 1, update_idle = 1;
static int reject_confirm_write;

static void record(const char *event)
{
    FILE *f = fopen(trace_path, "a"); assert(f);
    assert(fprintf(f, "%s\n", event) > 0); assert(fclose(f) == 0);
}
static void interrupt_at(const char *event)
{
    if (cut && !strcmp(cut, event)) _exit(77);
}
static int count(const char *event)
{
    char *text = NULL;
    if (!g_file_get_contents(trace_path, &text, NULL, NULL)) return 0;
    char **lines = g_strsplit(text, "\n", -1); int n = 0;
    for (size_t i = 0; lines[i]; ++i) n += !strcmp(lines[i], event);
    g_strfreev(lines); g_free(text); return n;
}
static gboolean flag(json_object *object, const char *key)
{
    json_object *value = NULL;
    return json_object_object_get_ex(object, key, &value) && json_object_get_boolean(value);
}
int __real_sl_json_write(const char *path, json_object *object);
int __real_sl_config_reset(sl_runtime *r);
int __wrap_sl_config_reset(sl_runtime *r)
{
    record("config");
    int ok = __real_sl_config_reset(r);
    if (ok) interrupt_at("config-written");
    return ok;
}
int __wrap_sl_json_write(const char *path, json_object *object)
{
    if (!strcmp(path, journal_path) && flag(object, "confirmed") && reject_confirm_write) return 0;
    int ok = __real_sl_json_write(path, object);
    if (ok) {
        if (!strcmp(path, runtime.config)) interrupt_at("config-written");
        if (!strcmp(path, journal_path)) {
            if (flag(object, "purge")) interrupt_at("purge-saved");
            if (flag(object, "clear")) interrupt_at("clear-saved");
            if (flag(object, "config")) interrupt_at("config-saved");
            if (flag(object, "confirmed")) interrupt_at("confirmed-saved");
            if (flag(object, "cloud_request_attempted")) interrupt_at("attempt-saved");
            interrupt_at("request-saved");
        }
    }
    return ok;
}
int __wrap_sl_http(const char *url, const char *body, const char *authorization,
                   unsigned timeout, char **response, long *status)
{
    assert(!strcmp(url, "http://127.0.0.1:18789/hoorii/device/unbind"));
    assert(!authorization && timeout == 12 && body);
    json_object *request = sl_json_parse(body, strlen(body)), *journal = sl_json_read(journal_path);
    assert(request && journal && !strcmp(sl_string(request, "action"), "unbind"));
    assert(!strcmp(sl_string(request, "requestId"), sl_string(journal, "requestId")));
    assert(!flag(journal, "confirmed"));
    assert(flag(journal, "cloud_request_attempted"));
    record("cloud");
    char *id_path = g_build_filename(root, "cloud-request-id", NULL), *id = NULL;
    if (g_file_get_contents(id_path, &id, NULL, NULL))
        assert(!strcmp(id, sl_string(request, "requestId")));
    else assert(g_file_set_contents(id_path, sl_string(request, "requestId"), -1, NULL));
    g_free(id); g_free(id_path);
    *status = cloud_ready ? 200 : 503;
    *response = g_strdup_printf("{\"success\":true,\"data\":{\"requestId\":\"%s\",\"action\":\"unbind\",\"hubId\":\"old\",\"allowReset\":true}}",
        sl_string(request, "requestId"));
    json_object_put(request); json_object_put(journal);
    interrupt_at("cloud-return");
    return 1;
}
json_object *__wrap_sl_update_call(const char *operation, json_object *args, char **code, char **error)
{
    assert(!strcmp(operation, "get_status") && !args && !code && !error);
    const char *json = update_idle ? "{\"maintenance_busy\":false}" : "{\"maintenance_busy\":true}";
    return sl_json_parse(json, strlen(json));
}
int __wrap_sl_run(const char *const argv[], unsigned timeout, char **output)
{
    assert(!output);
    json_object *journal = sl_json_read(journal_path); assert(journal);
    if (!strcmp(argv[0], "runuser") && !strcmp(argv[4], "env")) {
        assert(!flag(journal, "confirmed"));
        assert(timeout == 420 && !strcmp(argv[2], "pi"));
        assert(g_str_has_prefix(argv[5], "HOME=") && g_str_has_suffix(argv[5], "/user"));
        assert(!strcmp(argv[8], "HOORII_GATEWAY_REAL_USER=pi"));
        assert(g_str_has_suffix(argv[11], "/openclaw/hoorii_claw/script/startup.sh"));
        record("gateway"); json_object_put(journal); return gateway_ready;
    }
    assert(flag(journal, "confirmed")); json_object_put(journal);
    if (!strcmp(argv[0], "systemctl")) {
        assert(timeout == 30 && !strcmp(argv[1], "stop") && !strcmp(argv[2], "shadowlink-ble-server.service"));
        record("stop-ble"); return 1;
    }
    if (!strcmp(argv[0], "runuser")) {
        assert(timeout == 300 && g_str_has_suffix(argv[4], "/clear.sh"));
        record("clear"); interrupt_at("clear-effect"); return 1;
    }
    if (!strcmp(argv[0], "docker")) {
        assert(timeout == 60 && !strcmp(argv[1], "compose") && !strcmp(argv[5], "-d"));
        record("start-ha"); return 1;
    }
    assert(!strcmp(argv[0], "env") && timeout == 120 && !strcmp(argv[6], "--purge"));
    record("purge"); interrupt_at("purge-effect"); return 1;
}
int __wrap_sl_notify_unbind_reset(void)
{
    record("reset"); interrupt_at("reset-sent"); return ws_ready;
}
static void setup(void)
{
    root = g_dir_make_tmp("shadowlink-recovery-XXXXXX", NULL); assert(root);
    runtime = (sl_runtime){0};
    g_snprintf(runtime.release, sizeof(runtime.release), "%s/release", root);
    assert(!mkdir(runtime.release, 0700));
    g_snprintf(runtime.config, sizeof(runtime.config), "%s/config.json", runtime.release);
    const char *json = "{\"commissioning\":{\"passcode\":123},\"userinfo\":{\"hub_id\":\"old\",\"hub_token\":\"secret\"}}";
    json_object *config = sl_json_parse(json, strlen(json));
    assert(config && __real_sl_json_write(runtime.config, config)); json_object_put(config);
    char *current = g_build_filename(root, "current", NULL); assert(!symlink(runtime.release, current)); g_free(current);
    char *lock = g_build_filename(root, "lock", NULL), *marker = g_build_filename(root, "full-reset", NULL);
    g_setenv("CLAWSTAGE_STATE_DIR", root, TRUE);
    g_setenv("CLAWSTAGE_MAINTENANCE_LOCK", lock, TRUE);
    g_setenv("CLAWSTAGE_RESET_MARKER", marker, TRUE);
    g_free(lock); g_free(marker);
    journal_path = g_build_filename(runtime.release, ".device-unbind.json", NULL);
    trace_path = g_build_filename(root, "trace", NULL);
    cut = NULL; gateway_ready = cloud_ready = ws_ready = update_idle = 1; reject_confirm_write = 0;
}
static void teardown(void)
{
    unlink(journal_path); unlink(runtime.config); rmdir(runtime.release);
    const char *names[] = {"current", "lock", "trace", "cloud-request-id"};
    for (size_t i = 0; i < G_N_ELEMENTS(names); ++i) { char *p = g_build_filename(root, names[i], NULL); unlink(p); g_free(p); }
    assert(!rmdir(root)); g_free(root); g_free(journal_path); g_free(trace_path);
}
static void interrupt_operation(const char *at, gboolean local_only)
{
    pid_t pid = fork(); assert(pid >= 0);
    if (!pid) { cut = at; sl_unbind(&runtime, local_only); _exit(1); }
    int status; assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 77);
    assert(sl_unbind_pending(&runtime));
}
static void interrupt_live(const char *at) { interrupt_operation(at, FALSE); }
int main(void)
{
    const char *points[] = {"request-saved", "attempt-saved", "cloud-return", "confirmed-saved", "config-written", "config-saved",
                            "clear-effect", "clear-saved", "purge-effect", "purge-saved", "reset-sent"};
    for (size_t i = 0; i < G_N_ELEMENTS(points); ++i) {
        setup(); interrupt_live(points[i]);
        int cloud_before = count("cloud"), clear_before = count("clear"), purge_before = count("purge"), config_before = count("config");
        if (i >= 1) { gateway_ready = cloud_ready = 0; } /* Never repeat a recorded attempt. */
        assert(sl_recover_unbind(&runtime));
        assert(sl_unbind_pending(&runtime));
        assert(!sl_unbind_blocks_binding(&runtime));
        assert(count("gateway") == (i == 0 ? 1 : 0));
        assert(count("cloud") == cloud_before + (i == 0 ? 1 : 0));
        assert(count("config") == config_before + (i < 10 ? 1 : 0));
        assert(count("clear") == clear_before + (i < 10 ? 1 : 0));
        assert(count("purge") == purge_before + (i < 10 ? 1 : 0));
        assert(count("start-ha") == 1 && count("stop-ble") == (i == 10 ? 0 : 1));
        assert(sl_recover_unbind(&runtime)); /* Same-boot retries do not clear data or repeatedly reload the QR. */
        assert(count("start-ha") == 1);
        teardown();
    }
    /* Recover is local even if interrupted before its first confirmation.
     * Its intent must survive into the root boot recovery entry point. */
    const char *local_points[] = {"request-saved", "confirmed-saved", "config-written", "clear-saved", "purge-saved"};
    for (size_t i = 0; i < G_N_ELEMENTS(local_points); ++i) {
        setup(); interrupt_operation(local_points[i], TRUE);
        int config_before = count("config"), clear_before = count("clear"), purge_before = count("purge");
        gateway_ready = cloud_ready = 0;
        assert(count("cloud") == 0);
        assert(sl_recover_unbind(&runtime));
        assert(count("cloud") == 0 && count("gateway") == 0);
        assert(count("config") == config_before + 1 && count("clear") == clear_before + 1 && count("purge") == purge_before + 1);
        assert(!sl_unbind_blocks_binding(&runtime));
        teardown();
    }
    setup();
    assert(sl_unbind(&runtime, FALSE));
    assert(sl_unbind_pending(&runtime));
    int sent = count("reset");
    assert(sl_recover_unbind(&runtime) && count("reset") == sent);
    json_object *completed = sl_json_read(journal_path);
    json_object_object_add(completed, "reset_sent_boot_id", json_object_new_string("00000000-0000-4000-8000-000000000000"));
    assert(__real_sl_json_write(journal_path, completed)); json_object_put(completed);
    cloud_ready = gateway_ready = 0; ws_ready = 0;
    assert(!sl_recover_unbind(&runtime)); /* boot while renderer unavailable */
    assert(sl_unbind_pending(&runtime) && count("clear") == 1 && count("purge") == 1);
    ws_ready = 1;
    assert(sl_recover_unbind(&runtime) && count("reset") == sent + 2);
    assert(count("cloud") == 1 && count("gateway") == 0);
    assert(count("stop-ble") == 0); /* notification retries preserve rebind connections */
    teardown();

    setup(); interrupt_live("request-saved");
    gateway_ready = cloud_ready = 0;
    assert(sl_recover_unbind(&runtime) && count("cloud") == 1 && count("clear") == 1);
    assert(count("gateway") == 1 && count("stop-ble") == 1);
    teardown();

    setup(); interrupt_live("request-saved");
    cloud_ready = 0; reject_confirm_write = 1;
    assert(!sl_recover_unbind(&runtime) && count("clear") == 0 && count("stop-ble") == 0);
    assert(count("cloud") == 1);
    reject_confirm_write = 0; ws_ready = 0;
    assert(!sl_recover_unbind(&runtime) && count("clear") == 1 && sl_unbind_pending(&runtime));
    assert(count("cloud") == 1 && count("gateway") == 1);
    assert(!sl_unbind_blocks_binding(&runtime));
    int cloud_calls = count("cloud"), gateway_calls = count("gateway");
    ws_ready = 1; gateway_ready = cloud_ready = 0;
    assert(sl_recover_unbind(&runtime) && count("clear") == 1 && count("purge") == 1);
    assert(count("cloud") == cloud_calls && count("gateway") == gateway_calls);
    teardown();

    setup(); interrupt_live("confirmed-saved"); update_idle = 0;
    assert(!sl_recover_unbind(&runtime) && count("clear") == 0);
    update_idle = 1;
    int fd = sl_binding_lock(); assert(fd >= 0);
    assert(!sl_recover_unbind(&runtime) && count("clear") == 0); close(fd);
    char *old_config = NULL; assert(g_file_get_contents(runtime.config, &old_config, NULL, NULL));
    assert(g_file_set_contents(runtime.config, "{\"userinfo\":{\"hub_id\":\"new\",\"hub_token\":\"new-token\"}}", -1, NULL));
    assert(!sl_recover_unbind(&runtime) && count("clear") == 0 && sl_unbind_pending(&runtime));
    assert(g_file_set_contents(runtime.config, old_config, -1, NULL)); g_free(old_config);
    assert(g_file_set_contents(journal_path, "corrupt", -1, NULL));
    assert(!sl_recover_unbind(&runtime) && count("gateway") == 0 && count("clear") == 0);
    teardown();
    /* A late boot worker must only retire the old notification after rebind. */
    setup(); interrupt_live("reset-sent");
    assert(g_file_set_contents(runtime.config, "{\"userinfo\":{\"hub_id\":\"new\",\"hub_token\":\"new-token\"}}", -1, NULL));
    int resets = count("reset");
    assert(sl_recover_unbind(&runtime) && !sl_unbind_pending(&runtime));
    assert(count("clear") == 1 && count("purge") == 1 && count("reset") == resets);
    assert(count("stop-ble") == 0 && count("start-ha") == 0);
    json_object *config = sl_json_read(runtime.config), *userinfo = NULL;
    assert(json_object_object_get_ex(config, "userinfo", &userinfo));
    assert(!strcmp(sl_string(userinfo, "hub_id"), "new")); json_object_put(config);
    teardown();
    puts("PASS interrupted Shadowlink reset recovery");
    return 0;
}
