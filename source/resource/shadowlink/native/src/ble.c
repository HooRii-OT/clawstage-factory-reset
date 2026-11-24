#define _GNU_SOURCE
#include "runtime.h"
#include <glib-unix.h>
#include <gio/gunixfdlist.h>
#include <sys/socket.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define ROOT "/org/shadowlink"
#define SERVICE ROOT "/service0"
#define ADVERTISEMENT ROOT "/advertisement0"
#define AGENT ROOT "/agent"
#define AGENT_IFACE "org.bluez.Agent1"
#define AGENT_MANAGER "org.bluez.AgentManager1"
#define GATT_SERVICE "org.bluez.GattService1"
#define GATT_CHAR "org.bluez.GattCharacteristic1"
#define AD_IFACE "org.bluez.LEAdvertisement1"
#define PROPERTIES "org.freedesktop.DBus.Properties"
#define UUID_SERVICE "42d21ad8-8fb3-477a-bdce-8558dda5ea10"
#define UUID_AD "18221862-763a-4fa5-a38e-2a7595937a8b"
#define CHARACTERISTIC_COUNT 5
#define BUSY_READ_WINDOW_SECONDS 5
#define ADMISSION_ALLOWED 0x00
#define ADMISSION_DEVICE_BUSY 0x01

#define MAX_PEERS 2
#define MAX_QUEUED_FRAMES 16

typedef struct ble ble;
typedef struct peer peer;
typedef struct userinfo_job userinfo_job;
struct peer {
    ble *server;
    sl_runtime runtime, *r;
    char *path;
    gboolean connected, working, resync, notifying;
    gint64 connected_at, activity, last_report, indication_at;
    char report_transaction[128];
    int64_t report_sequence;
    int notify_fd;
    guint notify_source, mtu, link_generation, userinfo_callbacks;
    gboolean awaiting_confirmation;
    size_t send_offset;
    GQueue outgoing, requests;
    userinfo_job *userinfo;
};
struct ble {
    sl_runtime *r;
    GDBusConnection *bus;
    GMainLoop *loop;
    GDBusNodeInfo *info;
    GHashTable *connected, *rejected, *disconnecting;
    GPtrArray *peers;
    peer *operation_owner, *control_owner, *reporter;
    gboolean binding, ota_active, ota_known, probing;
    guint ota_generation;
    gboolean registered, registering, advertising, advertising_pending, unregistering, stopping;
    guint objects[9];
    gint64 adv_retry, health_at;
    double idle_seconds;
    gboolean mock_bus, reconciling, probe_pending, pairing_pending;
    guint connection_generation, snapshot_generation;
};
typedef struct { ble *server; char *path; } rejection;
struct userinfo_job {
    peer *server;
    GSubprocess *process;
    char *value, *hub;
    guint timeout, link_generation;
    gboolean cancelled, timed_out, success;
    int binding_lock;
};
static void dispatch_next(peer *p);
static void device_changed(ble *b, const char *path, gboolean connected);
static void release_operation(peer *p, const char *reason);
static const char xml[] =
"<node>"
"<interface name='org.freedesktop.DBus.ObjectManager'><method name='GetManagedObjects'><arg type='a{oa{sa{sv}}}' direction='out'/></method></interface>"
"<interface name='org.bluez.GattService1'><property name='UUID' type='s' access='read'/><property name='Primary' type='b' access='read'/><property name='Characteristics' type='ao' access='read'/></interface>"
"<interface name='org.bluez.GattCharacteristic1'>"
"<method name='ReadValue'><arg type='a{sv}' direction='in'/><arg type='ay' direction='out'/></method>"
"<method name='WriteValue'><arg type='ay' direction='in'/><arg type='a{sv}' direction='in'/></method>"
"<method name='AcquireNotify'><arg type='a{sv}' direction='in'/><arg type='h' direction='out'/><arg type='q' direction='out'/></method>"
"<method name='StartNotify'/><method name='StopNotify'/><method name='Confirm'/>"
"<property name='UUID' type='s' access='read'/><property name='Service' type='o' access='read'/><property name='Flags' type='as' access='read'/><property name='Notifying' type='b' access='read'/>"
"<property name='NotifyAcquired' type='b' access='read'/>"
"</interface>"
"<interface name='org.bluez.Agent1'>"
"<method name='Release'/><method name='Cancel'/>"
"<method name='RequestPinCode'><arg type='o' direction='in'/><arg type='s' direction='out'/></method>"
"<method name='DisplayPinCode'><arg type='o' direction='in'/><arg type='s' direction='in'/></method>"
"<method name='RequestPasskey'><arg type='o' direction='in'/><arg type='u' direction='out'/></method>"
"<method name='DisplayPasskey'><arg type='o' direction='in'/><arg type='u' direction='in'/><arg type='q' direction='in'/></method>"
"<method name='RequestConfirmation'><arg type='o' direction='in'/><arg type='u' direction='in'/></method>"
"<method name='RequestAuthorization'><arg type='o' direction='in'/></method>"
"<method name='AuthorizeService'><arg type='o' direction='in'/><arg type='s' direction='in'/></method>"
"</interface>"
"<interface name='org.bluez.LEAdvertisement1'><method name='Release'/>"
"<property name='Type' type='s' access='read'/><property name='ServiceData' type='a{sv}' access='read'/>"
"</interface></node>";
static int char_index(const char *path)
{
    for (int i = 0; i < CHARACTERISTIC_COUNT; ++i) { char expected[128]; snprintf(expected, sizeof(expected), SERVICE "/char%d", i); if (!strcmp(path, expected)) return i; }
    return -1;
}
static GVariant *bytes(const uint8_t *data, size_t n) { return g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, data, n, 1); }
static GVariant *properties(ble *b, const char *path)
{
    GVariantBuilder p; g_variant_builder_init(&p, G_VARIANT_TYPE("a{sv}"));
    if (!strcmp(path, SERVICE)) {
        g_variant_builder_add(&p, "{sv}", "UUID", g_variant_new_string(UUID_SERVICE));
        g_variant_builder_add(&p, "{sv}", "Primary", g_variant_new_boolean(TRUE));
        GVariantBuilder list; g_variant_builder_init(&list, G_VARIANT_TYPE("ao"));
        for (int i = 0; i < CHARACTERISTIC_COUNT; ++i) { char child[128]; snprintf(child, sizeof(child), SERVICE "/char%d", i); g_variant_builder_add(&list, "o", child); }
        g_variant_builder_add(&p, "{sv}", "Characteristics", g_variant_builder_end(&list));
    } else if (!strcmp(path, ADVERTISEMENT)) {
        json_object *config = sl_json_read(b->r->config), *value = NULL;
        int flag = 0;
        if (config) {
            json_object *c = sl_commissioning(config);
            if (json_object_object_get_ex(c, "commissioning_flag", &value)) flag = json_object_get_int(value) == 1;
        }
        /* Like the cached key and passcode, the discriminator belongs to the
         * identity validated at startup; never replace it with a fallback. */
        uint8_t data[10];
        int ok = config && sl_advertisement(b->r->discriminator, flag, b->r->responder_id, b->r->public_key, data);
        json_object_put(config);
        if (!ok) { g_variant_builder_clear(&p); return NULL; }
        GVariantBuilder service_data; g_variant_builder_init(&service_data, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&service_data, "{sv}", UUID_AD, bytes(data, sizeof(data)));
        g_variant_builder_add(&p, "{sv}", "Type", g_variant_new_string("peripheral"));
        g_variant_builder_add(&p, "{sv}", "ServiceData", g_variant_builder_end(&service_data));
    } else {
        int i = char_index(path);
        if (i >= 0) {
            char uuid[64]; snprintf(uuid, sizeof(uuid), "42d21ad8-8fb3-477a-bdce-8558dda5ea1%d", i + 1);
            const char *flags0[] = {"read", NULL}, *flags1[] = {"write", NULL}, *flags2[] = {"write", "write-without-response", NULL}, *flags3[] = {"indicate", NULL};
            const char *const *flags[] = {flags0, flags1, flags2, flags3, flags0};
            g_variant_builder_add(&p, "{sv}", "UUID", g_variant_new_string(uuid));
            g_variant_builder_add(&p, "{sv}", "Service", g_variant_new_object_path(SERVICE));
            g_variant_builder_add(&p, "{sv}", "Flags", g_variant_new_strv(flags[i], -1));
            gboolean acquired = FALSE;
            if (i == 3) {
                for (guint n = 0; n < b->peers->len; ++n) {
                    peer *client = g_ptr_array_index(b->peers, n);
                    acquired |= client->connected && client->notifying;
                }
                g_variant_builder_add(&p, "{sv}", "NotifyAcquired", g_variant_new_boolean(acquired));
            }
            g_variant_builder_add(&p, "{sv}", "Notifying", g_variant_new_boolean(acquired));
        }
    }
    return g_variant_builder_end(&p);
}
static void ready(ble *b, gboolean yes)
{
    if (!yes) { unlink(b->r->ready); return; }
    char *directory = g_path_get_dirname(b->r->ready);
    if (!g_mkdir_with_parents(directory, 0755)) g_file_set_contents(b->r->ready, "ready\n", -1, NULL);
    g_free(directory);
}
static void close_notify(peer *p)
{
    if (p->notify_source) { g_source_remove(p->notify_source); p->notify_source = 0; }
    if (p->notify_fd >= 0) close(p->notify_fd);
    p->notify_fd = -1; p->notifying = FALSE; p->awaiting_confirmation = FALSE; p->send_offset = 0;
    g_queue_clear_full(&p->outgoing, (GDestroyNotify)g_bytes_unref);
}
static gboolean pump_notify(peer *p)
{
    if (p->awaiting_confirmation || g_queue_is_empty(&p->outgoing)) return TRUE;
    GBytes *packet = g_queue_peek_head(&p->outgoing); gsize length;
    const uint8_t *data = g_bytes_get_data(packet, &length);
    size_t count = MIN(length - p->send_offset, MIN(p->mtu - 3, 512u));
    ssize_t sent = send(p->notify_fd, data + p->send_offset, count, MSG_NOSIGNAL);
    if (sent != (ssize_t)count) { close_notify(p); return FALSE; }
    p->send_offset += count; p->awaiting_confirmation = TRUE; p->indication_at = g_get_monotonic_time();
    p->activity = g_get_monotonic_time();
    return TRUE;
}
static gboolean notify_event(gint fd, GIOCondition condition, gpointer user)
{
    peer *p = user; uint8_t confirmation;
    if ((condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) || recv(fd, &confirmation, 1, MSG_DONTWAIT) != 1 || confirmation != 1) {
        p->notify_source = 0; close_notify(p); return G_SOURCE_REMOVE;
    }
    if (p->awaiting_confirmation) {
        p->awaiting_confirmation = FALSE;
        GBytes *packet = g_queue_peek_head(&p->outgoing);
        if (packet && p->send_offset == g_bytes_get_size(packet)) {
            g_bytes_unref(g_queue_pop_head(&p->outgoing)); p->send_offset = 0;
        }
    }
    return pump_notify(p) ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}
static gboolean send_data(peer *p, const uint8_t *data, size_t length)
{
    if (!p->connected || !p->notifying || !length) return FALSE;
    if (g_queue_get_length(&p->outgoing) >= MAX_QUEUED_FRAMES) { close_notify(p); return FALSE; }
    g_queue_push_tail(&p->outgoing, g_bytes_new(data, length));
    return pump_notify(p);
}
static void userinfo_child_setup(gpointer user)
{
    (void)user;
    setpgid(0, 0);
}
static void kill_userinfo(userinfo_job *job)
{
    const char *pid = g_subprocess_get_identifier(job->process);
    if (pid) kill(-(pid_t)g_ascii_strtoll(pid, NULL, 10), SIGKILL);
    g_subprocess_force_exit(job->process);
    g_subprocess_wait(job->process, NULL, NULL);
}
static gboolean userinfo_timeout(gpointer user)
{
    userinfo_job *job = user;
    job->timeout = 0; job->timed_out = TRUE;
    sl_debug("User Info background operation timed out");
    kill_userinfo(job);
    return G_SOURCE_REMOVE;
}
static void cancel_userinfo(void *context)
{
    peer *b = context;
    if (!b->userinfo) return;
    userinfo_job *job = b->userinfo;
    job->cancelled = TRUE;
    if (job->timeout) { g_source_remove(job->timeout); job->timeout = 0; }
    kill_userinfo(job);
    close(job->binding_lock); job->binding_lock = -1;
    b->userinfo = NULL; b->r->userinfo_busy = FALSE;
    sl_debug("User Info background operation cancelled");
}
static int share_userinfo_lock(void *context)
{
    peer *b = context;
    if (!b->userinfo || b->userinfo->link_generation != b->link_generation) return -1;
    /* Reuse this binding transaction's lock, without admitting another link
     * or releasing maintenance exclusion while the background script runs. */
    return dup(b->userinfo->binding_lock);
}
static gboolean finish_userinfo(gpointer user)
{
    userinfo_job *job = user; peer *b = job->server;
    if (b->working) return G_SOURCE_CONTINUE;
    if (!job->cancelled && !b->server->stopping && b->connected) {
        b->userinfo = NULL; b->r->userinfo_busy = FALSE;
        uint8_t response[SL_MAX_FRAME]; size_t length = 0;
        if (sl_userinfo_response(b->r, job->success, job->value, job->hub, response, &length))
            send_data(b, response, length);
        sl_flush_userinfo(b->r);
        sl_debug("User Info background finished success=%d binding_pending=%d commissioning_response_sent=%d",
            job->success, b->r->binding_pending, b->r->commissioning_response_sent);
        if (!b->r->binding_pending || b->r->commissioning_response_sent || !job->success)
            release_operation(b, job->success ? "userinfo_finished" : "userinfo_failed");
    }
    if (job->binding_lock >= 0) close(job->binding_lock);
    --b->userinfo_callbacks;
    g_object_unref(job->process); g_free(job->value); g_free(job->hub); g_free(job);
    return G_SOURCE_REMOVE;
}
static void userinfo_done(GObject *source, GAsyncResult *result, gpointer user)
{
    userinfo_job *job = user; GError *error = NULL;
    job->success = g_subprocess_wait_check_finish(G_SUBPROCESS(source), result, &error) && !job->timed_out;
    if (job->timeout) { g_source_remove(job->timeout); job->timeout = 0; }
    g_clear_error(&error);
    if (finish_userinfo(job) == G_SOURCE_CONTINUE) g_timeout_add(10, finish_userinfo, job);
}
static int defer_userinfo(void *context, const char *value, const char *hub)
{
    peer *b = context;
    GSubprocessLauncher *launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
    g_subprocess_launcher_set_child_setup(launcher, userinfo_child_setup, NULL, NULL);
    /* Hold maintenance exclusion only until systemd accepts the handoff.
     * The independent worker owns startup; disconnect cancels only this waiter. */
    int lock = dup(b->r->binding_lock_fd);
    if (lock < 0) { g_object_unref(launcher); return 0; }
    g_subprocess_launcher_take_fd(launcher, lock, 9);
    char *script = g_build_filename(b->r->release, "openclaw", "hoorii_claw", "script", "internal", "provision-runtime.py", NULL);
    GError *error = NULL;
    GSubprocess *process = g_subprocess_launcher_spawn(launcher, &error, "/usr/bin/python3", script, "enqueue", b->r->release, NULL);
    g_object_unref(launcher); g_free(script);
    if (!process) { sl_debug("User Info background launch failed"); g_clear_error(&error); return 0; }
    userinfo_job *job = g_new0(userinfo_job, 1);
    job->binding_lock = dup(b->r->binding_lock_fd);
    job->server = b; job->process = process; job->value = g_strdup(value); job->hub = g_strdup(hub);
    job->link_generation = b->link_generation;
    b->userinfo = job; b->r->userinfo_busy = TRUE; ++b->userinfo_callbacks;
    job->timeout = g_timeout_add_seconds(30, userinfo_timeout, job);
    g_subprocess_wait_check_async(process, NULL, userinfo_done, job);
    sl_debug("User Info background operation started");
    return 1;
}
static void unregistered(GObject *source, GAsyncResult *result, gpointer user)
{
    ble *b = user; GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    b->unregistering = FALSE;
    char *name = error ? g_dbus_error_get_remote_error(error) : NULL;
    if (reply || !g_strcmp0(name, "org.bluez.Error.DoesNotExist")) { b->advertising = FALSE; ready(b, FALSE); }
    else sl_debug("Advertisement unregister failed: %s", error ? error->message : "unknown");
    if (reply) g_variant_unref(reply);
    g_free(name); g_clear_error(&error);
}
static void unregister_adv(ble *b)
{
    if (b->advertising_pending) { ready(b, FALSE); return; }
    if (!b->advertising || b->unregistering) return;
    b->unregistering = TRUE;
    g_dbus_connection_call(b->bus, "org.bluez", b->r->adapter, "org.bluez.LEAdvertisingManager1", "UnregisterAdvertisement",
        g_variant_new("(o)", ADVERTISEMENT), NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, unregistered, b);
}
static void device_changed(ble *b, const char *path, gboolean connected);
static gboolean valid_peer(ble *b, const char *path)
{
    return path && g_str_has_prefix(path, b->r->adapter) &&
        g_str_has_prefix(path + strlen(b->r->adapter), "/dev_");
}
static void rejected_done(GObject *source, GAsyncResult *result, gpointer user)
{
    rejection *request = user; ble *b = request->server; GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (reply) {
        g_variant_unref(reply);
        if (g_hash_table_remove(b->rejected, request->path)) ++b->connection_generation;
    } else sl_debug("Busy peer disconnect failed: %s", error ? error->message : "unknown");
    g_hash_table_remove(b->disconnecting, request->path);
    g_clear_error(&error); g_free(request->path); g_free(request);
}
static void reject_peer(ble *b, const char *path)
{
    gint64 *deadline = g_hash_table_lookup(b->rejected, path);
    if (!deadline) {
        deadline = g_new(gint64, 1);
        *deadline = g_get_monotonic_time() + BUSY_READ_WINDOW_SECONDS * G_USEC_PER_SEC;
        g_hash_table_insert(b->rejected, g_strdup(path), deadline);
        ++b->connection_generation;
        sl_log("Shadowlink connection rejected: device busy (connection limit reached)\n");
        sl_debug("connection rejected code=DEVICE_BUSY reason=%s connected=%u limit=%u retained_peers=%u retained_limit=4",
            g_hash_table_size(b->connected) >= MAX_PEERS ? "connection_limit" :
            b->peers->len >= 4 ? "disconnected_peers_finishing" : "admission_pending",
            g_hash_table_size(b->connected), (unsigned)MAX_PEERS, b->peers->len);
    }
    /* Only S5 is readable during this fixed window. Repeated requests must
     * neither extend it nor disturb the owner's session and indications. */
    if (g_get_monotonic_time() < *deadline) return;
    if (g_hash_table_contains(b->disconnecting, path)) return;
    g_hash_table_add(b->disconnecting, g_strdup(path));
    rejection *request = g_new0(rejection, 1);
    request->server = b; request->path = g_strdup(path);
    g_dbus_connection_call(b->bus, "org.bluez", path, "org.bluez.Device1", "Disconnect",
        NULL, NULL, 0, 5000, NULL, rejected_done, request);
}
static gboolean admit_peer(ble *b, const char *path)
{
    if (!valid_peer(b, path)) return FALSE;
    if (g_hash_table_contains(b->connected, path)) return TRUE;
    if (g_hash_table_size(b->connected) >= MAX_PEERS || g_hash_table_contains(b->rejected, path) ||
        g_hash_table_contains(b->disconnecting, path)) {
        reject_peer(b, path); return FALSE;
    }
    device_changed(b, path, TRUE);
    return g_hash_table_contains(b->connected, path);
}
typedef struct {
    peer *p;
    GBytes *frame;
    uint8_t response[SL_MAX_FRAME];
    size_t length;
    int handled;
} frame_job;
static gboolean mutation(unsigned id)
{
    return id == 0 || id == 2 || id == 4 || id == 6 || id == 8 || id == 12 || id == 14 || id == 18 || id == 20;
}
static void release_operation(peer *p, const char *reason)
{
    if (p->server->operation_owner == p) {
        sl_debug("operation ownership released reason=%s binding=%d binding_pending=%d userinfo_busy=%d connected=%d",
            reason, p->server->binding, p->r->binding_pending, p->r->userinfo_busy, p->connected);
        p->server->operation_owner = NULL; p->server->binding = FALSE;
    }
}
static void finish_frame(frame_job *job)
{
    peer *p = job->p; ble *b = p->server;
    gsize n; const uint8_t *frame = g_bytes_get_data(job->frame, &n);
    gboolean sent = job->handled && job->length && send_data(p, job->response, job->length);
    if (p->r->after_action == 3 && sent) { b->ota_active = TRUE; b->reporter = p; ++b->ota_generation; }
    sl_after_response(p->r, sent);
    if (p->r->advertisement_changed) { unregister_adv(b); p->r->advertisement_changed = FALSE; }
    if (frame[0] == 0) { p->resync = TRUE; p->last_report = 0; }
    if (frame[0] == 5 && frame[1] == 0x15) p->last_report = 0;
    if (p->r->invalidate_peers) {
        p->r->invalidate_peers = FALSE;
        for (guint i = 0; i < b->peers->len; ++i) {
            peer *other = g_ptr_array_index(b->peers, i);
            if (other == p || !other->connected) continue;
            g_dbus_connection_call(b->bus, "org.bluez", other->path, "org.bluez.Device1", "Disconnect",
                NULL, NULL, 0, 5000, NULL, NULL, NULL);
            device_changed(b, other->path, FALSE);
        }
    }
    if (!p->connected) cancel_userinfo(p);
    if (b->operation_owner == p) {
        if (!b->binding || (!p->r->binding_pending && !p->userinfo) || !p->connected)
            release_operation(p, !p->connected ? "disconnected_worker_finished" : "request_finished");
    }
    if (b->control_owner == p) b->control_owner = NULL;
    p->working = FALSE;
    g_bytes_unref(job->frame); g_free(job);
    for (guint i = 0; i < b->peers->len; ++i) dispatch_next(g_ptr_array_index(b->peers, i));
}
static void frame_done(GObject *source, GAsyncResult *result, gpointer user)
{
    (void)source; (void)user;
    frame_job *job = g_task_propagate_pointer(G_TASK(result), NULL);
    finish_frame(job);
}
static void frame_worker(GTask *task, gpointer source, gpointer user, GCancellable *cancel)
{
    (void)source; (void)cancel;
    frame_job *job = user; gsize n; const uint8_t *frame = g_bytes_get_data(job->frame, &n);
    job->handled = sl_handle_frame(job->p->r, frame, n, job->response, &job->length);
    g_task_return_pointer(task, job, NULL);
}
static void dispatch_next(peer *p)
{
    ble *b = p->server;
    if (p->working || !p->connected || b->stopping || g_queue_is_empty(&p->requests)) return;
    GBytes *next = g_queue_peek_head(&p->requests); gsize n;
    const uint8_t *frame = g_bytes_get_data(next, &n);
    gboolean business = frame[0] == 5;
    unsigned id = frame[1];
    gboolean control = business && (id == 16 || id == 23);
    if (control && b->control_owner) return;
    p->r->operation_busy = business && mutation(id) &&
        (!b->ota_known || (b->operation_owner && b->operation_owner != p) ||
         (b->binding && b->operation_owner == p && id != 2 && id != 6 && id != 8) ||
         (b->ota_active && !(id == 20 && b->reporter == p)));
    if (p->r->operation_busy) {
        peer *owner = b->operation_owner;
        /* This is the gate snapshot, before authentication/payload validation.
         * handler.c separately logs when it actually sends OPERATION_BUSY. */
        sl_debug("operation gate busy command=0x%02x reason=%s ota_known=%d ota_active=%d probe_pending=%d "
                 "binding=%d owner=%s owner_connected=%d owner_working=%d owner_binding_pending=%d "
                 "owner_userinfo_busy=%d owner_userinfo_job=%d requester_binding_pending=%d reporter_self=%d",
            id, !b->ota_known ? "ota_state_unknown" :
            owner && owner != p ? "other_connection_operation" :
            b->binding && owner == p && id != 2 && id != 6 && id != 8 ? "binding_in_progress" : "ota_active",
            b->ota_known, b->ota_active, b->probe_pending, b->binding,
            !owner ? "none" : owner == p ? "self" : "other",
            owner ? owner->connected : 0, owner ? owner->working : 0,
            owner ? owner->r->binding_pending : 0, owner ? owner->r->userinfo_busy : 0,
            owner && owner->userinfo != NULL, p->r->binding_pending, b->reporter == p);
    }
    if (business && id == 21 && b->reporter != p) {
        g_bytes_unref(g_queue_pop_head(&p->requests)); dispatch_next(p); return;
    }
    if (business && mutation(id) && !p->r->operation_busy) {
        if (b->operation_owner != p || (!b->binding && (id == 6 || id == 8)))
            sl_debug("operation ownership acquired command=0x%02x binding=%d binding_pending=%d",
                id, b->binding || id == 6 || id == 8, p->r->binding_pending);
        b->operation_owner = p;
        if (id == 6 || id == 8) b->binding = TRUE;
    }
    if (!business && n > 4 && frame[4] != 0x9e && !p->userinfo) release_operation(p, "new_authentication");
    if (control) b->control_owner = p;
    frame_job *job = g_new0(frame_job, 1); job->p = p; job->frame = g_queue_pop_head(&p->requests);
    p->working = TRUE;
    /* Binding uses the existing asynchronous subprocess API on this context.
     * Slow device calls run in workers; each peer still processes in order. */
    if (!business || id == 2 || id == 6 || id == 8) {
        job->handled = sl_handle_frame(p->r, frame, n, job->response, &job->length);
        finish_frame(job);
    } else {
        GTask *task = g_task_new(NULL, NULL, frame_done, NULL);
        g_task_set_task_data(task, job, NULL); g_task_run_in_thread(task, frame_worker); g_object_unref(task);
    }
}
static int consume_frame(const uint8_t *frame, size_t n, void *user)
{
    peer *p = user;
    if (g_queue_get_length(&p->requests) >= MAX_QUEUED_FRAMES) {
        close_notify(p);
        g_dbus_connection_call(p->server->bus, "org.bluez", p->path, "org.bluez.Device1", "Disconnect",
            NULL, NULL, 0, 5000, NULL, NULL, NULL);
        device_changed(p->server, p->path, FALSE); return 0;
    }
    g_queue_push_tail(&p->requests, g_bytes_new(frame, n)); dispatch_next(p); return 1;
}
static void method(GDBusConnection *connection, const gchar *sender, const gchar *path, const gchar *interface,
                    const gchar *name, GVariant *parameters, GDBusMethodInvocation *invocation, gpointer user)
{
    (void)connection; (void)sender;
    ble *b = user;
    if (!strcmp(interface, AGENT_IFACE)) {
        if (!strcmp(name, "Release")) {
            sl_log("Bluetooth pairing rejection agent released; exiting for systemd recovery\n");
            ready(b, FALSE); b->stopping = TRUE; g_main_loop_quit(b->loop);
            g_dbus_method_invocation_return_value(invocation, NULL);
        } else if (!strcmp(name, "Cancel")) {
            g_dbus_method_invocation_return_value(invocation, NULL);
        } else {
            /* BLE authentication belongs to Shadowlink, never OS bonding.
             * Do not log device addresses, PINs or confirmation codes. */
            sl_log("Bluetooth system pairing rejected: %s\n", name);
            g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Rejected", "System Bluetooth pairing is disabled");
        }
        return;
    }
    if (!strcmp(name, "GetManagedObjects")) {
        GVariantBuilder objects; g_variant_builder_init(&objects, G_VARIANT_TYPE("a{oa{sa{sv}}}"));
        for (int i = -1; i < CHARACTERISTIC_COUNT; ++i) {
            char object[128]; if (i < 0) g_strlcpy(object, SERVICE, sizeof(object)); else snprintf(object, sizeof(object), SERVICE "/char%d", i);
            GVariantBuilder interfaces; g_variant_builder_init(&interfaces, G_VARIANT_TYPE("a{sa{sv}}"));
            g_variant_builder_add(&interfaces, "{s@a{sv}}", i < 0 ? GATT_SERVICE : GATT_CHAR, properties(b, object));
            g_variant_builder_add(&objects, "{o@a{sa{sv}}}", object, g_variant_builder_end(&interfaces));
        }
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(@a{oa{sa{sv}}})", g_variant_builder_end(&objects))); return;
    }
    if (!strcmp(name, "Release")) { b->advertising = FALSE; ready(b, FALSE); g_dbus_method_invocation_return_value(invocation, NULL); return; }
    int index = char_index(path);
    if (!strcmp(name, "AcquireNotify") && index == 3) {
        GVariant *options; const char *device = NULL; guint16 mtu = 23;
        g_variant_get(parameters, "(@a{sv})", &options);
        g_variant_lookup(options, "device", "&o", &device); g_variant_lookup(options, "mtu", "q", &mtu);
        if (!admit_peer(b, device)) { g_variant_unref(options); goto busy; }
        peer *p = g_hash_table_lookup(b->connected, device);
        g_variant_unref(options);
        if (mtu < 23 || mtu > 517 || p->notifying) goto invalid;
        int sockets[2];
        if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sockets)) goto invalid;
        GUnixFDList *fds = g_unix_fd_list_new();
        int handle = g_unix_fd_list_append(fds, sockets[1], NULL); close(sockets[1]);
        if (handle < 0) { close(sockets[0]); g_object_unref(fds); goto invalid; }
        p->notify_fd = sockets[0]; p->mtu = mtu; p->notifying = TRUE; p->resync = TRUE; p->last_report = 0;
        p->notify_source = g_unix_fd_add(p->notify_fd, G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL, notify_event, p);
        g_dbus_method_invocation_return_value_with_unix_fd_list(invocation, g_variant_new("(hq)", handle, mtu), fds);
        g_object_unref(fds); return;
    } else if ((!strcmp(name, "StartNotify") || !strcmp(name, "StopNotify") || !strcmp(name, "Confirm")) && index == 3) {
        /* No shared Value signal fallback: it can leak another phone's reply. */
        g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.NotSupported", "Per-device AcquireNotify required"); return;
    }
    else if (!strcmp(name, "ReadValue") && index == 4) {
        GVariant *options; const char *device = NULL; guint16 offset = 0;
        g_variant_get(parameters, "(@a{sv})", &options);
        g_variant_lookup(options, "device", "&o", &device);
        g_variant_lookup(options, "offset", "q", &offset);
        if (!valid_peer(b, device)) { g_variant_unref(options); goto busy; }
        uint8_t status[] = {0x53, 0x4c, 0x02, ADMISSION_ALLOWED, MAX_PEERS};
        if (offset > sizeof(status)) {
            g_variant_unref(options);
            g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.InvalidOffset", "Invalid admission status offset"); return;
        }
        if (!admit_peer(b, device)) status[3] = ADMISSION_DEVICE_BUSY;
        g_variant_unref(options);
        /* A read reply is scoped to the requesting ATT connection. Never
         * publish admission status on S4. */
        g_dbus_method_invocation_return_value(invocation,
            g_variant_new("(@ay)", bytes(status + offset, sizeof(status) - offset))); return;
    }
    else if (!strcmp(name, "ReadValue") && index == 0) {
        GVariant *options; const char *device = NULL;
        g_variant_get(parameters, "(@a{sv})", &options);
        g_variant_lookup(options, "device", "&o", &device);
        gboolean admitted = admit_peer(b, device);
        g_variant_unref(options);
        if (!admitted) goto busy;
        const uint8_t version[] = {0x53, 0x4c, 2, 2, 1, 0};
        /* Python always returns the complete supported-version value. */
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(@ay)", bytes(version, sizeof(version)))); return;
    } else if (!strcmp(name, "WriteValue") && (index == 1 || index == 2)) {
        GVariant *value, *options; const char *device = NULL; guint16 offset = 0;
        g_variant_get(parameters, "(@ay@a{sv})", &value, &options);
        g_variant_lookup(options, "device", "&o", &device); g_variant_lookup(options, "offset", "q", &offset);
        if (!admit_peer(b, device)) { g_variant_unref(value); g_variant_unref(options); goto busy; }
        /* Python records only business writes and successful outgoing data. */
        peer *p = g_hash_table_lookup(b->connected, device);
        if (index == 2) p->activity = g_get_monotonic_time();
        if (index == 2 && !p->notifying) { g_variant_unref(value); g_variant_unref(options); goto busy; }
        gint64 now = g_get_monotonic_time();
        gsize n; const uint8_t *data = g_variant_get_fixed_array(value, &n, 1);
        sl_debug("GATT WriteValue characteristic=%d bytes=%zu offset=%u buffered=%zu", index, (size_t)n, offset, p->r->receiver.length);
        if (index == 1 && n >= 2) sl_debug("selected protocol version=0x%02x%02x", data[0], data[1]);
        int ok = index == 1 || sl_receive(&p->r->receiver, data, n, offset, (uint64_t)(now / 1000), consume_frame, p);
        sl_debug("GATT receive accepted=%d buffered=%zu", ok, p->r->receiver.length);
        g_variant_unref(value); g_variant_unref(options);
        /* Python discards malformed fragments without a D-Bus write error. */
        if (!ok) sl_debug("GATT fragment discarded");
    } else goto invalid;
    g_dbus_method_invocation_return_value(invocation, NULL); return;
 busy:
    g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.NotPermitted",
        "Shadowlink device busy or peer unavailable"); return;
 invalid:
    sl_debug("GATT rejected method=%s characteristic=%d reason=invalid_arguments", name, index);
    g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.InvalidArguments", "Invalid Shadowlink operation or value"); return;
}
static GVariant *get_property(GDBusConnection *connection, const gchar *sender, const gchar *path, const gchar *interface,
                               const gchar *name, GError **error, gpointer user)
{
    (void)connection; (void)sender; (void)interface;
    GVariant *props = properties(user, path);
    if (!props) {
        ready(user, FALSE);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Shadowlink advertisement identity unavailable");
        return NULL;
    }
    g_variant_ref_sink(props);
    GVariant *value = g_variant_lookup_value(props, name, NULL); g_variant_unref(props); return value;
}
static const GDBusInterfaceVTable vtable = {.method_call = method, .get_property = get_property};
static void advertisement_done(GObject *source, GAsyncResult *result, gpointer user)
{
    ble *b = user; GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    b->advertising_pending = FALSE;
    char *name = error ? g_dbus_error_get_remote_error(error) : NULL;
    if (!b->stopping) {
        if (reply || !g_strcmp0(name, "org.bluez.Error.AlreadyExists")) {
            b->advertising = TRUE; puts("Advertisement registered successfully");
            ready(b, g_hash_table_size(b->connected) < MAX_PEERS);
            if (g_hash_table_size(b->connected) >= MAX_PEERS) unregister_adv(b);
        } else {
            ready(b, FALSE);
            sl_log("Advertisement registration failed: %s\n", error->message);
            if (strstr(error->message, "Failed to parse") || strstr(error->message, "Invalid")) {
                b->stopping = TRUE; g_main_loop_quit(b->loop);
            } else b->adv_retry = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
        }
    }
    if (reply) g_variant_unref(reply);
    g_free(name); g_clear_error(&error);
}
static void application_done(GObject *source, GAsyncResult *result, gpointer user)
{
    ble *b = user; GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    b->registering = FALSE;
    if (reply) { g_variant_unref(reply); b->registered = TRUE; puts("GATT application registered successfully"); }
    else { sl_log("GATT registration failed: %s\n", error->message); b->stopping = TRUE; g_main_loop_quit(b->loop); }
    g_clear_error(&error);
}
static void free_peer(gpointer user)
{
    peer *p = user; close_notify(p);
    g_queue_clear_full(&p->requests, (GDestroyNotify)g_bytes_unref);
    sl_session_reset(&p->r->session); g_free(p->r->pending_userinfo); g_free(p->path); g_free(p);
}
static void device_changed(ble *b, const char *path, gboolean connected)
{
    if (!valid_peer(b, path) || (b->stopping && connected)) return;
    peer *p = g_hash_table_lookup(b->connected, path);
    if (connected) {
        if (p) return;
        if (g_hash_table_size(b->connected) >= MAX_PEERS || b->peers->len >= 4 ||
            g_hash_table_contains(b->rejected, path) || g_hash_table_contains(b->disconnecting, path)) {
            reject_peer(b, path); return;
        }
        p = g_new0(peer, 1); p->server = b; p->runtime = *b->r; p->r = &p->runtime;
        p->r->userinfo_context = p; p->r->defer_userinfo = defer_userinfo;
        p->r->cancel_userinfo = cancel_userinfo; p->r->share_userinfo_lock = share_userinfo_lock;
        p->path = g_strdup(path); p->notify_fd = -1; p->connected = TRUE;
        p->connected_at = p->activity = g_get_monotonic_time(); p->resync = TRUE;
        g_ptr_array_add(b->peers, p); g_hash_table_insert(b->connected, g_strdup(path), p);
        ++b->connection_generation; b->adv_retry = 0;
        if (g_hash_table_size(b->connected) >= MAX_PEERS) unregister_adv(b);
    } else {
        ++b->connection_generation;
        if (g_hash_table_remove(b->rejected, path)) return;
        if (!p) return;
        if (b->reporter == p) b->reporter = NULL;
        p->connected = FALSE; ++p->link_generation; close_notify(p);
        g_queue_clear_full(&p->requests, (GDestroyNotify)g_bytes_unref);
        /* A running worker owns its runtime until completion. */
        if (!p->working) { cancel_userinfo(p); release_operation(p, "disconnected"); }
        else if (b->operation_owner == p)
            sl_debug("operation ownership retained reason=disconnected_worker_running binding=%d binding_pending=%d",
                b->binding, p->r->binding_pending);
        g_hash_table_remove(b->connected, path);
        b->adv_retry = g_get_monotonic_time() + G_USEC_PER_SEC;
    }
}
static GVariant *adapter_property(ble *b, const char *interface, const char *property)
{
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_sync(b->bus, "org.bluez", b->r->adapter, PROPERTIES, "Get",
        g_variant_new("(ss)", interface, property), G_VARIANT_TYPE("(v)"), 0, 5000, NULL, &error);
    GVariant *value = NULL;
    if (reply) { g_variant_get(reply, "(v)", &value); g_variant_unref(reply); }
    else sl_debug("Advertisement health check unavailable: %s", error ? error->message : "unknown");
    g_clear_error(&error); return value;
}
static void pairing_disabled(GObject *source, GAsyncResult *result, gpointer user)
{
    ble *b = user; GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    b->pairing_pending = FALSE;
    if (reply) g_variant_unref(reply);
    else if (!b->stopping)
        sl_log("Failed to disable Bluetooth pairing; health check will retry: %s\n", error ? error->message : "unknown");
    g_clear_error(&error);
}
static void disable_pairing(ble *b)
{
    if (b->stopping || b->pairing_pending) return;
    b->pairing_pending = TRUE;
    sl_log("Bluetooth Pairable is enabled; disabling system pairing\n");
    g_dbus_connection_call(b->bus, "org.bluez", b->r->adapter, PROPERTIES, "Set",
        g_variant_new("(ssv)", "org.bluez.Adapter1", "Pairable", g_variant_new_boolean(FALSE)),
        NULL, 0, 5000, NULL, pairing_disabled, b);
}
static gboolean setup_pairing_policy(ble *b, GError **error)
{
    /* The first/default BlueZ agent sets IO capability and can enable bonding.
     * Own a rejecting default agent before GATT/advertising, then turn pairing
     * off. A later desktop RegisterAgent must not take over this policy. */
    const char *methods[] = {"RegisterAgent", "RequestDefaultAgent", "Set"};
    for (guint i = 0; i < G_N_ELEMENTS(methods); ++i) {
        GVariant *args = i == 0 ? g_variant_new("(os)", AGENT, "NoInputNoOutput") :
            i == 1 ? g_variant_new("(o)", AGENT) :
            g_variant_new("(ssv)", "org.bluez.Adapter1", "Pairable", g_variant_new_boolean(FALSE));
        GVariant *reply = g_dbus_connection_call_sync(b->bus, "org.bluez", i == 2 ? b->r->adapter : "/org/bluez",
            i == 2 ? PROPERTIES : AGENT_MANAGER, methods[i], args, NULL, 0, 5000, NULL, error);
        if (!reply) return FALSE;
        g_variant_unref(reply);
    }
    return TRUE;
}
static void check_health(ble *b)
{
    gint64 now = g_get_monotonic_time();
    /* Also reconcile when both BLE slots are occupied or a signal was lost. */
    GVariant *pairable = adapter_property(b, "org.bluez.Adapter1", "Pairable");
    if (pairable) {
        if (g_variant_get_boolean(pairable)) disable_pairing(b);
        g_variant_unref(pairable);
    }
    GList *paths = g_hash_table_get_keys(b->connected);
    for (GList *entry = paths; entry; entry = entry->next) {
        const char *path = entry->data;
        peer *p = g_hash_table_lookup(b->connected, path);
        if (p->working || p->userinfo) continue;
        gboolean ota_ready = p->r->ota_authenticated && p->notifying;
        gboolean stale = b->ota_active ? !ota_ready && now - p->connected_at >= 60 * G_USEC_PER_SEC :
            now - p->activity >= b->idle_seconds * G_USEC_PER_SEC;
        if (!stale) continue;
        sl_debug("BLE disconnect requested: idle/authentication timeout session=%d ota_active=%d", p->r->session.state, b->ota_active);
        GError *error = NULL;
        GVariant *reply = g_dbus_connection_call_sync(b->bus, "org.bluez", path, "org.bluez.Device1", "Disconnect",
            NULL, NULL, 0, 5000, NULL, &error);
        /* Like Python, a successful method reply is sufficient even if the
         * corresponding PropertiesChanged signal is lost. */
        if (reply) { g_variant_unref(reply); device_changed(b, path, FALSE); }
        else sl_debug("BLE Disconnect failed: %s", error ? error->message : "unknown");
        g_clear_error(&error);
    }
    g_list_free(paths);
    if (g_hash_table_size(b->connected) >= MAX_PEERS) return;
    GVariant *powered = adapter_property(b, "org.bluez.Adapter1", "Powered");
    GVariant *instances = adapter_property(b, "org.bluez.LEAdvertisingManager1", "ActiveInstances");
    if (powered && instances) {
        if (!g_variant_get_boolean(powered)) {
            g_dbus_connection_call(b->bus, "org.bluez", b->r->adapter, PROPERTIES, "Set",
                g_variant_new("(ssv)", "org.bluez.Adapter1", "Powered", g_variant_new_boolean(TRUE)), NULL, 0, 5000, NULL, NULL, NULL);
        } else if (b->advertising && !g_variant_get_byte(instances) && now >= b->adv_retry) {
            sl_debug("Advertisement health check detected stale registration");
            unregister_adv(b);
            b->advertising = FALSE; ready(b, FALSE);
        }
    }
    if (powered) g_variant_unref(powered);
    if (instances) g_variant_unref(instances);
}
static void connections_done(GObject *source, GAsyncResult *result, gpointer user)
{
    ble *b = user; GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    b->reconciling = FALSE;
    /* A live signal received while this snapshot was in flight wins. */
    if (reply && !b->stopping && b->snapshot_generation == b->connection_generation) {
        GHashTable *actual = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        GVariant *objects; GVariantIter iter; const char *path; GVariant *interfaces;
        g_variant_get(reply, "(@a{oa{sa{sv}}})", &objects);
        g_variant_iter_init(&iter, objects);
        while (g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &path, &interfaces)) {
            GVariant *device = g_variant_lookup_value(interfaces, "org.bluez.Device1", G_VARIANT_TYPE("a{sv}"));
            gboolean connected = FALSE;
            if (valid_peer(b, path) && device && g_variant_lookup(device, "Connected", "b", &connected) && connected) {
                g_hash_table_add(actual, g_strdup(path));
            }
            if (device) g_variant_unref(device);
            g_variant_unref(interfaces);
        }
        GList *known = g_hash_table_get_keys(b->connected);
        for (GList *entry = known; entry; entry = entry->next)
            if (!g_hash_table_contains(actual, entry->data)) {
                sl_debug("BLE health check detected missing connection");
                device_changed(b, entry->data, FALSE);
            }
        g_list_free(known);
        known = g_hash_table_get_keys(b->rejected);
        for (GList *entry = known; entry; entry = entry->next)
            if (!g_hash_table_contains(actual, entry->data)) device_changed(b, entry->data, FALSE);
        g_list_free(known);
        /* Remove stale owners before admitting peers from the snapshot. */
        GHashTableIter peers; gpointer peer;
        g_hash_table_iter_init(&peers, actual);
        while (g_hash_table_iter_next(&peers, &peer, NULL))
            if (!g_hash_table_contains(b->connected, peer)) device_changed(b, peer, TRUE);
        g_hash_table_destroy(actual); g_variant_unref(objects);
    } else if (error) sl_debug("BLE connection health check failed: %s", error->message);
    if (reply) g_variant_unref(reply);
    g_clear_error(&error);
    if (!b->stopping) check_health(b);
}
static void signal_received(GDBusConnection *connection, const gchar *sender, const gchar *path,
                            const gchar *interface, const gchar *name, GVariant *parameters, gpointer user)
{
    (void)connection; (void)sender; (void)interface;
    ble *b = user;
    if (!strcmp(name, "NameOwnerChanged")) {
        const char *service, *old, *next; g_variant_get(parameters, "(&s&s&s)", &service, &old, &next);
        (void)next;
        if (!strcmp(service, "org.bluez") && *old) {
            sl_debug("BlueZ owner changed; exiting for systemd recovery");
            ready(b, FALSE); b->stopping = TRUE; g_main_loop_quit(b->loop);
        }
        return;
    }
    if (!strcmp(name, "PropertiesChanged")) {
        const char *changed_interface; GVariant *changed, *invalid;
        g_variant_get(parameters, "(&s@a{sv}@as)", &changed_interface, &changed, &invalid);
        gboolean connected;
        if (!strcmp(changed_interface, "org.bluez.Device1") && g_variant_lookup(changed, "Connected", "b", &connected)) device_changed(b, path, connected);
        if (!strcmp(path, b->r->adapter) && !strcmp(changed_interface, "org.bluez.Adapter1")) {
            gboolean pairable = FALSE;
            if (g_variant_lookup(changed, "Pairable", "b", &pairable) && pairable) disable_pairing(b);
        }
        g_variant_unref(changed); g_variant_unref(invalid);
    } else if (!strcmp(name, "InterfacesRemoved")) {
        const char *object; GVariant *removed; g_variant_get(parameters, "(&o@as)", &object, &removed);
        GVariantIter iter; const char *removed_interface;
        g_variant_iter_init(&iter, removed);
        while (g_variant_iter_next(&iter, "&s", &removed_interface))
            if (!strcmp(removed_interface, "org.bluez.Device1")) device_changed(b, object, FALSE);
        g_variant_unref(removed);
    } else if (!strcmp(name, "InterfacesAdded")) {
        const char *object; GVariant *interfaces; g_variant_get(parameters, "(&o@a{sa{sv}})", &object, &interfaces);
        GVariant *device = g_variant_lookup_value(interfaces, "org.bluez.Device1", G_VARIANT_TYPE("a{sv}")); gboolean connected;
        if (device && g_variant_lookup(device, "Connected", "b", &connected)) device_changed(b, object, connected);
        if (device) g_variant_unref(device);
        g_variant_unref(interfaces);
    }
}
static void ota_tick(peer *b)
{
    if (!b->notifying && !b->r->ota_active && !*b->r->pending_activation && !b->server->probe_pending) return;
    char *code = NULL; json_object *snapshot = sl_update_call("get_status", NULL, &code, NULL);
    if (code || !snapshot) { b->server->probe_pending = TRUE; g_free(code); json_object_put(snapshot); return; }
    json_object *value = NULL; json_object_object_get_ex(snapshot, "recovery_in_progress", &value);
    if (json_object_get_boolean(value)) {
        sl_debug("OTA state confirmation deferred reason=controller_recovering ota_known=%d ota_active=%d",
            b->server->ota_known, b->server->ota_active);
        b->server->probe_pending = TRUE; json_object_put(snapshot); return;
    }
    if (!b->server->ota_known) sl_debug("OTA state confirmed recovery_in_progress=0");
    b->server->probe_pending = FALSE; b->server->ota_known = TRUE;
    if (*b->r->pending_activation) {
        json_object_object_get_ex(snapshot, "accepted", &value);
        if (strcmp(sl_string(snapshot, "transaction_id"), b->r->pending_activation) || json_object_get_boolean(value)) b->r->pending_activation[0] = 0;
        else { b->r->after_action = 3; sl_after_response(b->r, TRUE); }
    }
    json_object_object_get_ex(snapshot, "ota_protocol_version", &value); int version = value ? json_object_get_int(value) : 2;
    json_object_object_get_ex(snapshot, "accepted", &value);
    const char *status = sl_string(snapshot, "status");
    b->server->ota_active = b->r->ota_active = (version < 3 || json_object_get_boolean(value)) && *status && strcmp(status, "IDLE") && strcmp(status, "SUCCEEDED") && strcmp(status, "FAILED");
    if (!b->notifying || !b->r->ota_authenticated) { json_object_put(snapshot); return; }
    json_object *event = NULL, *args = NULL, *events = NULL; const char *legacy = NULL;
    if (version < 3) {
        json_object *session = NULL; json_object_object_get_ex(snapshot, "session", &session);
        legacy = sl_string(session, "session_id");
        if (!*legacy) { json_object_put(snapshot); return; }
        args = json_object_new_object(); json_object_object_add(args, "session_id", json_object_new_string(legacy));
        json_object_object_get_ex(session, "last_received_sequence", &value);
        json_object_object_add(args, "after_sequence", json_object_new_int64(json_object_get_int64(value)));
        events = sl_update_call("get_events", args, NULL, NULL);
        if (json_object_is_type(events, json_type_array) && json_object_array_length(events)) event = json_object_get(json_object_array_get_idx(events, 0));
    } else {
        if (b->resync) {
            json_object *result = sl_update_call("resync_status", NULL, &code, NULL);
            gboolean synced = !code || !strcmp(code, "UNKNOWN_OPERATION");
            if (synced) { b->resync = FALSE; b->last_report = 0; }
            json_object_put(result); g_free(code); code = NULL;
            if (!synced) { json_object_put(snapshot); return; }
        }
        event = sl_update_call("get_pending_status", NULL, NULL, NULL);
    }
    if (event) {
        json_object_object_get_ex(event, "sequence", &value); int64_t sequence = json_object_get_int64(value);
        const char *transaction = sl_string(event, "transaction_id");
        gint64 now = g_get_monotonic_time();
        if (legacy || sequence != b->report_sequence || strcmp(transaction, b->report_transaction) || now - b->last_report >= 5 * G_USEC_PER_SEC) {
            json_object *config = sl_json_read(b->r->config), *ui = NULL; if (config) json_object_object_get_ex(config, "userinfo", &ui);
            const char *hub = sl_string(ui, "hub_id"); uint8_t output[SL_MAX_FRAME]; size_t length;
            if (*hub && sl_update_report(event, hub, legacy, output, &length) && send_data(b, output, length)) {
                sl_debug("OTA status push sequence=%lld status=%s legacy=%d", (long long)sequence, sl_string(event, "status"), legacy != NULL);
                b->last_report = now; b->report_sequence = sequence; g_strlcpy(b->report_transaction, transaction, sizeof(b->report_transaction));
                if (legacy) {
                    json_object_object_add(args, "sequence", json_object_new_int64(sequence)); json_object_object_del(args, "after_sequence");
                    json_object *ack = sl_update_call("acknowledge", args, NULL, NULL); json_object_put(ack);
                }
            }
            json_object_put(config);
        }
    }
    json_object_put(event); json_object_put(events); json_object_put(args); json_object_put(snapshot);
}
typedef struct { ble *server; guint generation; json_object *snapshot; } ota_probe;
static void probe_worker(GTask *task, gpointer source, gpointer user, GCancellable *cancel)
{
    (void)source; (void)cancel;
    ota_probe *probe = user;
    probe->snapshot = sl_update_call("get_status", NULL, NULL, NULL);
    g_task_return_pointer(task, probe, NULL);
}
static void probe_done(GObject *source, GAsyncResult *result, gpointer user)
{
    (void)source; (void)user;
    ota_probe *probe = g_task_propagate_pointer(G_TASK(result), NULL); ble *b = probe->server;
    b->probing = FALSE;
    if (probe->snapshot && probe->generation == b->ota_generation) {
        json_object *value = NULL;
        json_object_object_get_ex(probe->snapshot, "recovery_in_progress", &value);
        if (json_object_get_boolean(value))
            sl_debug("OTA state confirmation deferred reason=controller_recovering ota_known=%d ota_active=%d",
                b->ota_known, b->ota_active);
        else if (!b->ota_known) sl_debug("OTA state confirmed recovery_in_progress=0");
        b->ota_known = !json_object_get_boolean(value); b->probe_pending = !b->ota_known;
        json_object_object_get_ex(probe->snapshot, "ota_protocol_version", &value);
        int version = value ? json_object_get_int(value) : 2;
        json_object_object_get_ex(probe->snapshot, "accepted", &value);
        const char *status = sl_string(probe->snapshot, "status");
        b->ota_active = (version < 3 || json_object_get_boolean(value)) && *status &&
            strcmp(status, "IDLE") && strcmp(status, "SUCCEEDED") && strcmp(status, "FAILED");
    }
    json_object_put(probe->snapshot); g_free(probe);
}
static gboolean tick(gpointer user)
{
    ble *b = user; gint64 now = g_get_monotonic_time();
    if (b->stopping) return G_SOURCE_CONTINUE;
    if (!b->reporter && !b->probing && (b->probe_pending || b->ota_active)) {
        ota_probe *probe = g_new0(ota_probe, 1); probe->server = b; probe->generation = b->ota_generation;
        b->probing = TRUE;
        GTask *task = g_task_new(NULL, NULL, probe_done, NULL);
        g_task_set_task_data(task, probe, NULL); g_task_run_in_thread(task, probe_worker); g_object_unref(task);
    }
    GHashTableIter rejected; gpointer rejected_path;
    g_hash_table_iter_init(&rejected, b->rejected);
    while (g_hash_table_iter_next(&rejected, &rejected_path, NULL)) reject_peer(b, rejected_path);
    if (!b->reconciling && now >= b->health_at) {
        b->reconciling = TRUE; b->health_at = now + 15 * G_USEC_PER_SEC;
        b->snapshot_generation = b->connection_generation;
        g_dbus_connection_call(b->bus, "org.bluez", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects",
            NULL, G_VARIANT_TYPE("(a{oa{sa{sv}}})"), 0, 5000, NULL, connections_done, b);
    }
    if (!b->registered && !b->registering && now >= b->adv_retry) {
        if (!b->mock_bus) {
            g_dbus_connection_call(b->bus, "org.bluez", b->r->adapter, PROPERTIES, "Set", g_variant_new("(ssv)", "org.bluez.Adapter1", "Powered", g_variant_new_boolean(TRUE)), NULL, 0, 5000, NULL, NULL, NULL);
            g_dbus_connection_call(b->bus, "org.bluez", b->r->adapter, PROPERTIES, "Set", g_variant_new("(ssv)", "org.bluez.Adapter1", "Discoverable", g_variant_new_boolean(FALSE)), NULL, 0, 5000, NULL, NULL, NULL);
        }
        b->registering = TRUE;
        g_dbus_connection_call(b->bus, "org.bluez", b->r->adapter, "org.bluez.GattManager1", "RegisterApplication", g_variant_new("(o@a{sv})", ROOT, g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0)), NULL, 0, 10000, NULL, application_done, b);
    }
    if (b->registered && !b->reconciling && !b->advertising && !b->advertising_pending && !b->unregistering && g_hash_table_size(b->connected) < MAX_PEERS && now >= b->adv_retry) {
        b->advertising_pending = TRUE;
        g_dbus_connection_call(b->bus, "org.bluez", b->r->adapter, "org.bluez.LEAdvertisingManager1", "RegisterAdvertisement", g_variant_new("(o@a{sv})", ADVERTISEMENT, g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0)), NULL, 0, 10000, NULL, advertisement_done, b);
    }
    for (guint i = 0; i < b->peers->len;) {
        peer *p = g_ptr_array_index(b->peers, i);
        if (p->connected && p->awaiting_confirmation && now - p->indication_at > 30 * G_USEC_PER_SEC) {
            g_dbus_connection_call(b->bus, "org.bluez", p->path, "org.bluez.Device1", "Disconnect",
                NULL, NULL, 0, 5000, NULL, NULL, NULL);
            device_changed(b, p->path, FALSE);
        }
        if (!p->connected && !p->working) cancel_userinfo(p);
        if (!p->connected && !p->working && !p->userinfo_callbacks) {
            release_operation(p, "peer_cleanup"); g_ptr_array_remove_index(b->peers, i); continue;
        }
        ++i;
    }
    if (b->reporter && !b->reporter->notifying) b->reporter = NULL;
    /* OTA controller acknowledgements are global: one authenticated reporter.
     * Other connected phones retain normal request/reply access. */
    for (guint i = 0; i < b->peers->len; ++i) {
        peer *p = g_ptr_array_index(b->peers, i);
        if (!b->reporter && p->connected && !p->working && p->notifying && p->r->ota_authenticated) {
            b->reporter = p; p->resync = TRUE; p->last_report = 0;
        }
        if (b->reporter == p && p->connected && !p->working) { ota_tick(p); break; }
    }
    return G_SOURCE_CONTINUE;
}
static gboolean stop(gpointer user) { ble *b = user; b->stopping = TRUE; g_main_loop_quit(b->loop); return G_SOURCE_REMOVE; }
static gboolean supported_bluez(void)
{
    /* Older BlueZ can accept AcquireNotify but broadcast its FD to all peers.
     * Do not silently run the concurrent transport on those implementations. */
    const char *paths[] = {"/usr/libexec/bluetooth/bluetoothd", "/usr/lib/bluetooth/bluetoothd"};
    char *output = NULL;
    for (guint i = 0; i < G_N_ELEMENTS(paths); ++i) {
        if (!g_file_test(paths[i], G_FILE_TEST_IS_EXECUTABLE)) continue;
        if (!sl_run((const char *const[]){paths[i], "--version", NULL}, 2, &output)) { g_free(output); return FALSE; }
        unsigned major = 0, minor = 0;
        gboolean supported = output && sscanf(output, "%u.%u", &major, &minor) == 2 &&
            (major > 5 || (major == 5 && minor >= 82));
        g_free(output); return supported;
    }
    return FALSE;
}
int sl_ble_run(sl_runtime *r, gboolean session_bus)
{
    if (!session_bus && !supported_bluez()) {
        sl_log("Shadowlink concurrent BLE requires BlueZ >= 5.82; shared notification fallback is disabled\n");
        unlink(r->ready); return 0;
    }
    ble b = {.r = r, .idle_seconds = 180, .mock_bus = session_bus, .probe_pending = TRUE};
    /* Reconcile links already present after a daemon restart before advertising. */
    b.health_at = 0;
    GError *error = NULL;
    b.bus = g_bus_get_sync(session_bus ? G_BUS_TYPE_SESSION : G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!b.bus) { sl_log("D-Bus unavailable: %s\n", error->message); g_clear_error(&error); return 0; }
    const char *idle = g_getenv("SHADOWLINK_GATT_IDLE_TIMEOUT_SECONDS");
    if (idle) {
        char *end = NULL; double seconds = g_ascii_strtod(idle, &end);
        if (end != idle && !*end) b.idle_seconds = (seconds >= 10.0 ? seconds : 10.0);
    }
    b.info = g_dbus_node_info_new_for_xml(xml, &error);
    b.loop = g_main_loop_new(NULL, FALSE); b.connected = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    b.peers = g_ptr_array_new_with_free_func(free_peer);
    b.rejected = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    b.disconnecting = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    ready(&b, FALSE);
    int ok = 1;
    const char *paths[] = {ROOT, SERVICE, SERVICE "/char0", SERVICE "/char1", SERVICE "/char2", SERVICE "/char3", SERVICE "/char4", ADVERTISEMENT, AGENT};
    const char *interfaces[] = {"org.freedesktop.DBus.ObjectManager", GATT_SERVICE, GATT_CHAR, GATT_CHAR, GATT_CHAR, GATT_CHAR, GATT_CHAR, AD_IFACE, AGENT_IFACE};
    for (size_t i = 0; i < G_N_ELEMENTS(paths); ++i) {
        b.objects[i] = g_dbus_connection_register_object(b.bus, paths[i], g_dbus_node_info_lookup_interface(b.info, interfaces[i]), &vtable, &b, NULL, &error);
        if (!b.objects[i]) { ok = 0; break; }
    }
    guint signals[4];
    signals[0] = g_dbus_connection_signal_subscribe(b.bus, "org.bluez", PROPERTIES, "PropertiesChanged", NULL, NULL, 0, signal_received, &b, NULL);
    signals[1] = g_dbus_connection_signal_subscribe(b.bus, "org.bluez", "org.freedesktop.DBus.ObjectManager", "InterfacesAdded", NULL, NULL, 0, signal_received, &b, NULL);
    signals[2] = g_dbus_connection_signal_subscribe(b.bus, "org.bluez", "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved", NULL, NULL, 0, signal_received, &b, NULL);
    signals[3] = g_dbus_connection_signal_subscribe(b.bus, "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged", NULL, "org.bluez", 0, signal_received, &b, NULL);
    if (ok && !setup_pairing_policy(&b, &error)) ok = 0;
    guint timer = g_timeout_add_seconds(1, tick, &b), sigterm = g_unix_signal_add(SIGTERM, stop, &b), sigint = g_unix_signal_add(SIGINT, stop, &b);
    if (ok) { tick(&b); g_main_loop_run(b.loop); }
    if (error) { sl_log("Bluetooth D-Bus setup failed: %s\n", error->message); g_clear_error(&error); }
    b.stopping = TRUE;
    g_source_remove(timer);
    for (guint i = 0; i < b.peers->len; ++i) {
        peer *p = g_ptr_array_index(b.peers, i); p->connected = FALSE; close_notify(p);
    }
    gboolean pending;
    do {
        pending = FALSE;
        for (guint i = 0; i < b.peers->len; ++i) {
            peer *p = g_ptr_array_index(b.peers, i);
            if (!p->working) cancel_userinfo(p);
            pending |= p->working || p->userinfo_callbacks;
        }
        if (pending) g_main_context_iteration(NULL, TRUE);
    } while (pending);
    if (g_main_context_find_source_by_id(NULL, sigterm)) g_source_remove(sigterm);
    if (g_main_context_find_source_by_id(NULL, sigint)) g_source_remove(sigint);
    ready(&b, FALSE);
    /* Closing the dedicated bus connection unregisters all objects in BlueZ. */
    for (size_t i = 0; i < G_N_ELEMENTS(signals); ++i) g_dbus_connection_signal_unsubscribe(b.bus, signals[i]);
    for (size_t i = 0; i < G_N_ELEMENTS(b.objects); ++i) if (b.objects[i]) g_dbus_connection_unregister_object(b.bus, b.objects[i]);
    g_dbus_connection_close_sync(b.bus, NULL, NULL);
    /* Drain callbacks before freeing the stack-owned server state. */
    while (b.probing || b.registering || b.advertising_pending || b.unregistering || b.reconciling || b.pairing_pending || g_hash_table_size(b.disconnecting)) g_main_context_iteration(NULL, TRUE);
    r->userinfo_context = NULL; r->defer_userinfo = NULL; r->cancel_userinfo = NULL;
    r->share_userinfo_lock = NULL;
    g_object_unref(b.bus); g_main_loop_unref(b.loop); g_dbus_node_info_unref(b.info); g_hash_table_destroy(b.connected);
    g_hash_table_destroy(b.rejected); g_hash_table_destroy(b.disconnecting); g_ptr_array_free(b.peers, TRUE);
    return ok;
}
