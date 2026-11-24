#include "device.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 0; } } while (0)
static int http_failure, ws_failure;
static char request_id[40];
static int clear_calls, purge_calls, ws_calls, failures;
static int http_calls;
static int fail_next_write;
int __real_sl_json_write(const char *path, json_object *object);
int __wrap_sl_json_write(const char *path, json_object *object)
{
    if (fail_next_write) { fail_next_write = 0; return 0; }
    return __real_sl_json_write(path, object);
}
static const char *expected_action = "unbind";
int __wrap_sl_http(const char *url, const char *body, const char *authorization,
                   unsigned timeout, char **response, long *status)
{
    CHECK(!strcmp(url, "http://127.0.0.1:18789/hoorii/device/unbind"));
    CHECK(!authorization && timeout == 12 && clear_calls == 0);
    json_object *request = sl_json_parse(body, strlen(body));
    CHECK(!strcmp(sl_string(request, "action"), expected_action));
    http_calls++;
    const char *id = sl_string(request, "requestId");
    CHECK(g_uuid_string_is_valid(id));
    if (*request_id) CHECK(!strcmp(request_id, id));
    g_strlcpy(request_id, id, sizeof(request_id));
    *status = http_failure == 1 ? 202 : http_failure == 6 ? 503 : http_failure == 7 ? 403 : 200;
    *response = g_strdup_printf("{\"success\":%s,\"data\":{\"requestId\":\"%s\",\"action\":\"unbind\",\"hubId\":\"%s\",\"allowReset\":true}}",
        http_failure == 2 ? "\"true\"" : "true", http_failure == 3 ? "old" : id, http_failure == 4 ? "other" : "old");
    if (http_failure >= 8) {
        g_free(*response);
        *response = http_failure == 8 ? g_strdup("not JSON") : http_failure == 9 ? NULL :
            g_strdup("{\"success\":false,\"data\":{\"allowReset\":false}}");
    }
    json_object_put(request); return http_failure != 5;
}
int __wrap_sl_run(const char *const argv[], unsigned timeout, char **output)
{
    (void)output;
    if (!strcmp(argv[0], "systemctl")) { CHECK(timeout == 5 && !strcmp(argv[2], "--no-block")); return 1; }
    if (!strcmp(argv[0], "runuser")) {
        CHECK(timeout == 300 && !strcmp(argv[2], "pi") && g_str_has_suffix(argv[4], "/openclaw/hoorii_claw/script/clear.sh"));
        clear_calls++;
        return !failures;
    }
    CHECK(clear_calls > 0);
    CHECK(!strcmp(argv[0], "env") && !strcmp(argv[1], "--chdir"));
    CHECK(timeout == 120 && !strcmp(argv[3], "HOME=/home/pi") && !strcmp(argv[4], "SUDO_USER=pi"));
    CHECK(!strcmp(argv[6], "--purge") && !strcmp(argv[7], "all") && !strcmp(argv[9], "--no-pull"));
    CHECK(!strcmp(argv[10], "--system-dir") && !strcmp(argv[11], argv[2]));
    CHECK(!strcmp(argv[12], "--data-dir") && argv[14] == NULL);
    purge_calls++;
    return !failures;
}
int __wrap_sl_notify_unbind_reset(void)
{
    CHECK(purge_calls > 0);
    ws_calls++;
    return !failures && !ws_failure;
}
typedef enum { BOUND, UNBOUND, MISSING_TOKEN } BindingFixture;

static int run(gboolean preserve_session, int failure, gboolean factory, BindingFixture binding)
{
    char *directory = g_dir_make_tmp("shadowlink-unbind-XXXXXX", NULL);
    CHECK(directory);
    sl_runtime r = {0};
    g_strlcpy(r.release, directory, sizeof(r.release));
    g_snprintf(r.config, sizeof(r.config), "%s/config.json", directory);
    CHECK(g_file_set_contents(r.config, "{\"commissioning\":{\"commissioning_flag\":1,\"kpersistent\":\"secret\",\"passcode\":123},\"userinfo\":{\"hub_id\":\"old\",\"hub_token\":\"secret\"}}", -1, NULL));
    if (binding == UNBOUND) CHECK(g_file_set_contents(r.config,
        "{\"commissioning\":{\"passcode\":123},\"userinfo\":{}}", -1, NULL));
    if (binding == MISSING_TOKEN) CHECK(g_file_set_contents(r.config,
        "{\"commissioning\":{\"commissioning_flag\":1,\"passcode\":123},\"userinfo\":{\"hub_id\":\"old\"}}", -1, NULL));
    char *lock = g_build_filename(directory, "lock", NULL);
    g_setenv("CLAWSTAGE_MAINTENANCE_LOCK", lock, TRUE);
    if (geteuid() == 0) CHECK(chown(r.config, 1000, 1000) == 0);
    http_failure = failure;
    http_calls = ws_failure = clear_calls = purge_calls = ws_calls = failures = 0; request_id[0] = 0;
    expected_action = factory ? "factory_reset" : "unbind";
    memset(&r.session, 0x5a, sizeof(r.session));
    r.session.state = 2; r.ota_authenticated = TRUE; r.last_status = 2;
    sl_session session = r.session;
    if (factory) {
        int fd = sl_binding_lock(); CHECK(fd >= 0);
        CHECK(sl_authorize_unbind(&r, "factory_reset"));
        /* factory-reset.sh and --reset-config-only enter preparation separately. */
        CHECK(sl_authorize_unbind(&r, "factory_reset"));
        CHECK(sl_config_reset(&r));
        CHECK(sl_authorize_unbind(&r, "factory_reset"));
        CHECK(http_calls == 1);
        close(fd);
        goto cleanup;
    }
    CHECK(sl_unbind(&r, preserve_session));
    CHECK(http_calls == (preserve_session ? 0 : 1));
    CHECK(clear_calls == 1 && purge_calls == 1 && ws_calls == 1);
    CHECK(preserve_session ? !memcmp(&session, &r.session, sizeof(session)) : r.session.state == 0);
    CHECK(r.ota_authenticated == preserve_session);
    json_object *config = sl_json_read(r.config), *c = sl_commissioning(config), *ui = NULL;
    CHECK(!strcmp(sl_string(c, "kpersistent"), ""));
    CHECK(json_object_get_int(json_object_object_get(c, "commissioning_flag")) == 0);
    CHECK(json_object_get_int(json_object_object_get(c, "passcode")) == 123);
    CHECK(json_object_object_get_ex(config, "userinfo", &ui) && !strcmp(sl_string(ui, "hub_id"), ""));
    json_object_put(config);
    /* Simulate commissioning's retirement before an explicit new operation. */
    int binding_fd = sl_binding_lock(); CHECK(binding_fd >= 0);
    CHECK(sl_unbind_pending(&r) && sl_retire_completed_unbind(&r));
    close(binding_fd);
    r.session = session; r.ota_authenticated = TRUE; r.last_status = 2;
    failures = 1; clear_calls = purge_calls = ws_calls = 0; request_id[0] = 0;
    CHECK(g_file_set_contents(r.config, "{\"userinfo\":{\"hub_id\":\"old\",\"hub_token\":\"secret\"}}", -1, NULL));
    CHECK(!sl_unbind(&r, preserve_session));
    CHECK(http_calls == (preserve_session ? 0 : 2));
    CHECK(clear_calls == 2 && purge_calls == 2 && ws_calls == 0);
    CHECK(preserve_session ? !memcmp(&session, &r.session, sizeof(session)) : r.session.state == 0);
    CHECK(r.ota_authenticated == preserve_session);
    CHECK(r.last_status == (preserve_session ? 2 : -1));
    failures = 0; ws_failure = 1;
    CHECK(sl_unbind(&r, preserve_session));
    CHECK(http_calls == (preserve_session ? 0 : 2)); /* Local retries never repeat the cloud request. */
    CHECK(clear_calls == 3 && purge_calls == 3 && ws_calls == 1);
    CHECK(!sl_unbind_blocks_binding(&r));
    ws_failure = 0;
    CHECK(sl_unbind_pending(&r));
    r.last_status = -1; r.recovered = 0; // reconnect/boot lost transient status
    if (preserve_session) {
        CHECK(g_file_set_contents(r.config, "{\"userinfo\":{\"hub_id\":\"new-owner\",\"hub_token\":\"new-token\",\"union_id\":\"new-user\"}}", -1, NULL));
    }
    CHECK(sl_resume_unbind(&r));
    CHECK(http_calls == (preserve_session ? 0 : 2));
    CHECK(sl_unbind_pending(&r) == !preserve_session);
    CHECK(clear_calls == 3 && purge_calls == 3 && ws_calls == (preserve_session ? 1 : 2));
    if (preserve_session) {
        json_object *after = sl_json_read(r.config);
        CHECK(!strcmp(sl_string(json_object_object_get(after, "userinfo"), "hub_id"), "new-owner"));
        json_object_put(after);
    }
    CHECK(preserve_session ? !memcmp(&session, &r.session, sizeof(session)) : r.session.state == 0);
 cleanup:;
    char *journal = g_build_filename(directory, ".device-unbind.json", NULL);
    struct stat owner, journal_stat;
    if (sl_unbind_pending(&r)) {
        CHECK(!stat(r.config, &owner) && !stat(journal, &journal_stat));
        CHECK(owner.st_uid == journal_stat.st_uid && (journal_stat.st_mode & 0777) == 0600);
    }
    unlink(journal); g_free(journal); unlink(lock); g_free(lock);
    unlink(r.config); rmdir(directory); g_free(directory);
    return 1;
}
/* Full reset must take over both completed and interrupted BLE transactions.
 * HTTP failures never prevent local clearing; retries send no second request. */
static int takeover_test(int complete, int attempted)
{
    char *directory = g_dir_make_tmp("shadowlink-takeover-XXXXXX", NULL);
    CHECK(directory);
    sl_runtime r = {0};
    g_strlcpy(r.release, directory, sizeof(r.release));
    g_snprintf(r.config, sizeof(r.config), "%s/config.json", directory);
    CHECK(g_file_set_contents(r.config, "{\"commissioning\":{\"commissioning_flag\":0},\"userinfo\":{}}", -1, NULL));
    char *path = g_build_filename(directory, ".device-unbind.json", NULL);
    json_object *old = sl_json_parse("{\"requestId\":\"e6f72e99-ec48-4b64-8ba5-1037374445d4\",\"action\":\"unbind\",\"identity\":{\"hubId\":\"previous-owner\",\"tokenHash\":\"\"}}", strlen("{\"requestId\":\"e6f72e99-ec48-4b64-8ba5-1037374445d4\",\"action\":\"unbind\",\"identity\":{\"hubId\":\"previous-owner\",\"tokenHash\":\"\"}}"));
    CHECK(old);
    json_object_object_add(old, "confirmed", json_object_new_boolean(complete));
    json_object_object_add(old, "cleanup_complete", json_object_new_boolean(complete));
    json_object_object_add(old, "cloud_request_attempted", json_object_new_boolean(attempted));
    json_object_object_add(old, "local_only", json_object_new_boolean(TRUE));
    CHECK(sl_json_write(path, old));
    char *lock = g_build_filename(directory, "lock", NULL);
    g_setenv("CLAWSTAGE_MAINTENANCE_LOCK", lock, TRUE);
    int fd = sl_binding_lock(); CHECK(fd >= 0);
    http_calls = clear_calls = 0; request_id[0] = 0;
    http_failure = 5; expected_action = "factory_reset";
    fail_next_write = 1;
    CHECK(!sl_authorize_unbind(&r, "factory_reset"));
    CHECK(http_calls == 0);
    json_object *unchanged = sl_json_read(path);
    CHECK(unchanged && json_object_equal(unchanged, old));
    json_object_put(unchanged);
    CHECK(sl_authorize_unbind(&r, "factory_reset"));
    CHECK(http_calls == 1);
    json_object *saved = sl_unbind_journal(&r), *value = NULL;
    CHECK(saved && !strcmp(sl_string(saved, "action"), "factory_reset"));
    CHECK(strcmp(sl_string(saved, "requestId"), sl_string(old, "requestId")));
    CHECK(!json_object_object_get_ex(saved, "cleanup_complete", &value));
    CHECK(!json_object_object_get_ex(saved, "local_only", &value));
    CHECK(sl_config_reset(&r));
    CHECK(sl_authorize_unbind(&r, "factory_reset"));
    CHECK(http_calls == 1);
    json_object_put(saved); json_object_put(old); close(fd);
    unlink(path); unlink(r.config); unlink(lock); rmdir(directory);
    g_free(path); g_free(lock); g_free(directory);
    return 1;
}
static int legacy_config_reset_test(gboolean nested)
{
    char *directory = g_dir_make_tmp("shadowlink-legacy-reset-XXXXXX", NULL);
    CHECK(directory);
    sl_runtime r = {0};
    g_snprintf(r.config, sizeof(r.config), "%s/config.json", directory);
    json_object *config = json_object_new_object();
    json_object *commission = nested ? json_object_new_object() : config;
    if (nested) json_object_object_add(config, "commissioning", commission);
    json_object_object_add(commission, "passcode", json_object_new_int(12345678));
    json_object_object_add(commission, "commission_flag", json_object_new_int(1));
    json_object_object_add(config, "commission_flag", json_object_new_int(1));
    json_object_object_add(config, "commissioning_flag", json_object_new_int(1));
    json_object_object_add(config, "kpersistent", json_object_new_string("legacy-session"));
    CHECK(sl_json_write(r.config, config));
    json_object_put(config);
    CHECK(sl_config_reset(&r));
    config = sl_json_read(r.config); CHECK(config);
    commission = sl_commissioning(config);
    CHECK(json_object_get_int(json_object_object_get(commission, "passcode")) == 12345678);
    CHECK(json_object_get_int(json_object_object_get(commission, "commissioning_flag")) == 0);
    CHECK(!*sl_string(commission, "kpersistent"));
    CHECK(!json_object_object_get_ex(config, "commission_flag", NULL));
    CHECK(!json_object_object_get_ex(commission, "commission_flag", NULL));
    if (nested) {
        CHECK(!json_object_object_get_ex(config, "commissioning_flag", NULL));
        CHECK(!json_object_object_get_ex(config, "kpersistent", NULL));
    }
    CHECK(sl_commissioning_status(config) == 1);
    json_object_put(config);
    unlink(r.config); rmdir(directory); g_free(directory);
    return 1;
}
int main(void)
{
    if (!legacy_config_reset_test(FALSE) || !legacy_config_reset_test(TRUE)) return 1;
    for (int completed = 0; completed <= 1; ++completed)
        for (int attempted = 0; attempted <= 1; ++attempted)
            if (!takeover_test(completed, attempted)) return 1;
    for (int failure = 0; failure <= 10; ++failure)
        for (BindingFixture binding = BOUND; binding <= UNBOUND; ++binding)
            if (!run(FALSE, failure, FALSE, binding) || !run(TRUE, failure, FALSE, binding) ||
                !run(FALSE, failure, TRUE, binding)) return 1;
    return run(FALSE, 0, FALSE, MISSING_TOKEN) ? 0 : 1;
}
