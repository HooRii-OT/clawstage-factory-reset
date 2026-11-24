#include "device.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static int binding_lock(int mode)
{
    const char *path = g_getenv("CLAWSTAGE_MAINTENANCE_LOCK");
    if (!path || !*path) path = "/run/lock/clawstage-maintenance.lock";
    int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    struct stat st;
    if (fd >= 0 && (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & 0077) || flock(fd, mode | LOCK_NB))) { close(fd); fd = -1; }
    return fd;
}
int sl_binding_lock(void) { return binding_lock(LOCK_EX); }
/* Commission/HA token writes may finish while runtime startup holds a shared
 * maintenance lease. Identity replacement, reset and OTA remain exclusive. */
int sl_binding_shared_lock(void) { return binding_lock(LOCK_SH); }

static json_object *identity(sl_runtime *r)
{
    json_object *config = sl_json_read(r->config), *ui = NULL;
    if (!config) return NULL;
    json_object_object_get_ex(config, "userinfo", &ui);
    json_object *result = json_object_new_object();
    const char *token = sl_string(ui, "hub_token");
    char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, token, -1);
    json_object_object_add(result, "hubId", json_object_new_string(sl_string(ui, "hub_id")));
    json_object_object_add(result, "tokenHash", json_object_new_string(*token ? hash : ""));
    g_free(hash); json_object_put(config); return result;
}
static int true_field(json_object *object, const char *key)
{
    json_object *value = NULL;
    return object && json_object_object_get_ex(object, key, &value) &&
        json_object_is_type(value, json_type_boolean) && json_object_get_boolean(value);
}
gboolean sl_unbind_pending(sl_runtime *r)
{
    char *path = g_build_filename(r->release, ".device-unbind.json", NULL);
    struct stat st;
    gboolean pending = lstat(path, &st) == 0 || errno != ENOENT;
    g_free(path);
    return pending;
}

/* Recovery must not interpret an unreadable/corrupt journal as a new request. */
json_object *sl_unbind_journal(sl_runtime *r)
{
    char *path = g_build_filename(r->release, ".device-unbind.json", NULL);
    struct stat st, owner;
    json_object *saved = NULL, *snapshot = NULL, *confirmed = NULL, *attempted = NULL, *hub = NULL, *token = NULL, *local = NULL;
    if (lstat(path, &st) || !S_ISREG(st.st_mode) || (st.st_mode & 0077) ||
        (st.st_uid != geteuid() && (stat(r->config, &owner) || st.st_uid != owner.st_uid))) goto done;
    saved = sl_json_read(path);
    if (saved && g_uuid_string_is_valid(sl_string(saved, "requestId")) &&
        (!strcmp(sl_string(saved, "action"), "unbind") || !strcmp(sl_string(saved, "action"), "factory_reset")) &&
        json_object_object_get_ex(saved, "identity", &snapshot) &&
        json_object_object_get_ex(snapshot, "hubId", &hub) && json_object_is_type(hub, json_type_string) &&
        json_object_object_get_ex(snapshot, "tokenHash", &token) && json_object_is_type(token, json_type_string) &&
        (strlen(sl_string(snapshot, "tokenHash")) == 64 || !*sl_string(snapshot, "tokenHash")) &&
        (!json_object_object_get_ex(saved, "confirmed", &confirmed) || json_object_is_type(confirmed, json_type_boolean)) &&
        (!json_object_object_get_ex(saved, "cloud_request_attempted", &attempted) || json_object_is_type(attempted, json_type_boolean)) &&
        (!json_object_object_get_ex(saved, "local_only", &local) || json_object_is_type(local, json_type_boolean))) goto done;
    json_object_put(saved); saved = NULL;
 done:
    g_free(path);
    return saved;
}
int sl_unbind_identity_matches(sl_runtime *r, json_object *journal)
{
    json_object *current = identity(r), *snapshot = NULL;
    json_object_object_get_ex(journal, "identity", &snapshot);
    int matches = current && (json_object_equal(current, snapshot) ||
        (true_field(journal, "confirmed") && !*sl_string(current, "hubId") && !*sl_string(current, "tokenHash")));
    json_object_put(current);
    return matches;
}
/* A completed cleanup only owes a best-effort RESET notification. */
gboolean sl_unbind_blocks_binding(sl_runtime *r)
{
    json_object *saved = sl_unbind_journal(r);
    gboolean completed = true_field(saved, "confirmed") && true_field(saved, "cleanup_complete") &&
        !strcmp(sl_string(saved, "action"), "unbind");
    json_object_put(saved);
    return sl_unbind_pending(r) && !completed;
}
/* Caller holds the maintenance lock throughout preparation and cleanup.
 * Keep the legacy entry point and confirmed flag for existing lifecycle readers:
 * confirmed commits LOCAL cleanup; it no longer asserts cloud approval. */
static int prepare_unbind(sl_runtime *r, const char *action, gboolean local_only)
{
    char *path = g_build_filename(r->release, ".device-unbind.json", NULL);
    json_object *current = identity(r), *saved = sl_unbind_journal(r), *snapshot = NULL;
    json_object *request = NULL, *after = NULL;
    char *response = NULL; int ok = 0;
    const char *failure = "cannot persist local cleanup intent";
    if (!current) { failure = "binding config is missing, unreadable or invalid"; goto done; }
    /* An explicit full reset supersedes a previous BLE unbind, including
     * interrupted cleanup. Publish a NEW request atomically: never reuse an
     * unbind request ID with a different action or inherit completed steps.
     * The caller owns the maintenance lock and durable full-reset intent.
     * Until the replacement write succeeds the old journal remains intact. */
    gboolean takeover = saved && !strcmp(action, "factory_reset") &&
        !strcmp(sl_string(saved, "action"), "unbind");
    if (takeover) {
        sl_debug("Full reset supersedes BLE unbind requestId=%s cleanup_complete=%d; starting a new factory_reset transaction",
            sl_string(saved, "requestId"), true_field(saved, "cleanup_complete"));
        json_object_put(saved);
        saved = NULL;
    }
    if (saved) {
        json_object_object_get_ex(saved, "identity", &snapshot);
        if (strcmp(sl_string(saved, "action"), action)) { failure = "journal action does not match requested action"; goto done; }
        if (!json_object_equal(snapshot, current)) {
            /* Only our already-authorized clearing may have removed credentials. */
            if (true_field(saved, "confirmed") && !*sl_string(current, "hubId") &&
                !*sl_string(current, "tokenHash")) { ok = 1; goto done; }
            /* Keep the inhibitor on identity mismatch; never resume against a new owner. */
            failure = "journal binding identity differs from the current owner";
            goto done;
        }
        if (true_field(saved, "confirmed")) {
            sl_debug("Local cleanup already authorized requestId=%s; cloud request skipped", sl_string(saved, "requestId"));
            ok = 1; goto done;
        }
    } else {
        if (!takeover && sl_unbind_pending(r)) { failure = "existing journal is unreadable, unsafe or invalid"; goto done; }
        saved = json_object_new_object();
        char *id = g_uuid_string_random();
        json_object_object_add(saved, "requestId", json_object_new_string(id)); g_free(id);
        json_object_object_add(saved, "action", json_object_new_string(action));
        json_object_object_add(saved, "identity", json_object_get(current));
        /* Persist Recover's policy with the first intent write, so even an
         * interruption before confirmed cannot turn it into a cloud unbind. */
        if (local_only) json_object_object_add(saved, "local_only", json_object_new_boolean(TRUE));
        if (!sl_json_write(path, saved)) { failure = "cannot persist initial reset journal"; goto done; }
    }
    /* The user Gateway must observe local cleanup intent before config is cleared.
     * Preserve 0600; the account-config owner already owns this release. */
    struct stat owner;
    if (stat(r->config, &owner) || chown(path, owner.st_uid, owner.st_gid)) {
        sl_debug("Cannot set reset journal ownership: %s", strerror(errno));
        failure = "cannot read config ownership or apply it to reset journal";
        goto done;
    }
    if (local_only) {
        json_object_object_add(saved, "local_only", json_object_new_boolean(TRUE));
        if (!sl_json_write(path, saved)) goto done;
    }
    if (!true_field(saved, "local_only") && !true_field(saved, "cloud_request_attempted")) {
        request = json_object_new_object();
        json_object_object_add(request, "requestId", json_object_new_string(sl_string(saved, "requestId")));
        json_object_object_add(request, "action", json_object_new_string(action));
        char *config_path = g_build_filename(r->release, "user", ".openclaw", "openclaw.json", NULL);
        json_object *config = sl_json_read(config_path), *gateway = NULL, *port_value = NULL;
        g_free(config_path);
        int port = 18789;
        if (config && json_object_object_get_ex(config, "gateway", &gateway) &&
            json_object_object_get_ex(gateway, "port", &port_value)) {
            port = json_object_is_type(port_value, json_type_int) ? json_object_get_int(port_value) : 0;
        }
        json_object_put(config);
        if (port < 1 || port > 65535) port = 18789;
        char *url = g_strdup_printf("http://127.0.0.1:%d/hoorii/device/unbind", port);
        /* Persist before sending: retries/resume must not repeat this best-effort
         * request, even if the process dies while waiting for its response. */
        json_object_object_add(saved, "cloud_request_attempted", json_object_new_boolean(TRUE));
        if (!sl_json_write(path, saved)) { failure = "cannot persist cloud_request_attempted"; g_free(url); goto done; }
        sl_debug("Cloud unbind requestId=%s action=%s endpoint=%s timeout=12s (best effort)",
            sl_string(saved, "requestId"), action, url);
        long status = 0;
        int transported = sl_http(url, json_object_to_json_string(request), NULL, 12, &response, &status);
        g_free(url);
        sl_log("Cloud unbind attempted (transport=%d HTTP=%ld); continuing local cleanup regardless of response\n",
            transported, status);
    }
    after = identity(r);
    if (!after || !json_object_equal(current, after)) {
        failure = "binding config became unreadable or owner changed during request";
        goto done;
    }
    json_object_object_add(saved, "confirmed", json_object_new_boolean(TRUE));
    ok = sl_json_write(path, saved);
 done:
    if (!ok) sl_log("Cannot prepare local cleanup: %s; journal=%s config=%s\n", failure, path, r->config);
    json_object_put(current); json_object_put(saved); json_object_put(request);
    json_object_put(after); g_free(response); g_free(path);
    return ok;
}
int sl_authorize_unbind(sl_runtime *r, const char *action)
{
    return prepare_unbind(r, action, FALSE);
}
int sl_prepare_recover(sl_runtime *r)
{
    return prepare_unbind(r, "unbind", TRUE);
}
