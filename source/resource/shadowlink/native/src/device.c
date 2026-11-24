#define _GNU_SOURCE
#include "device.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#define RUN(seconds, ...) sl_run((const char *const[]){__VA_ARGS__, NULL}, seconds, NULL)
#define CAPTURE(out, seconds, ...) sl_run((const char *const[]){__VA_ARGS__, NULL}, seconds, out)

static int audio(const char *const arguments[], char **output)
{
    struct passwd *entry;
    uid_t best = 65534; char *user = NULL;
    setpwent();
    while ((entry = getpwent())) {
        char path[64]; snprintf(path, sizeof(path), "/run/user/%u", (unsigned)entry->pw_uid);
        if (entry->pw_uid >= 1000 && entry->pw_uid < best &&
            strcmp(entry->pw_shell, "/usr/sbin/nologin") && strcmp(entry->pw_shell, "/bin/false") &&
            g_file_test(path, G_FILE_TEST_IS_DIR)) { best = entry->pw_uid; g_free(user); user = g_strdup(entry->pw_name); }
    }
    endpwent();
    if (!user) return 0;
    char runtime[128], bus[160];
    snprintf(runtime, sizeof(runtime), "XDG_RUNTIME_DIR=/run/user/%u", (unsigned)best);
    snprintf(bus, sizeof(bus), "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/%u/bus", (unsigned)best);
    const char *command[24]; size_t n = 0;
    if (!geteuid()) { command[n++] = "runuser"; command[n++] = "-u"; command[n++] = user; command[n++] = "--"; }
    command[n++] = "env"; command[n++] = runtime; command[n++] = bus;
    for (size_t i = 0; arguments[i] && n < G_N_ELEMENTS(command) - 1; ++i) command[n++] = arguments[i];
    command[n] = NULL;
    int ok = sl_run(command, 5, output); g_free(user); return ok;
}
int sl_volume(int percent)
{
    char value[32], *text = NULL;
    if (percent >= 0) {
        if (percent > 100) return -1;
        snprintf(value, sizeof(value), "%.2f", percent / 100.0);
        if (audio((const char *const[]){"wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", value, NULL}, NULL))
            return audio((const char *const[]){"wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", percent ? "0" : "1", NULL}, NULL) ? percent : -1;
        snprintf(value, sizeof(value), "%d%%", percent);
        return audio((const char *const[]){"pactl", "set-sink-volume", "@DEFAULT_SINK@", value, NULL}, NULL) &&
            audio((const char *const[]){"pactl", "set-sink-mute", "@DEFAULT_SINK@", percent ? "0" : "1", NULL}, NULL) ? percent : -1;
    }
    if (audio((const char *const[]){"wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@", NULL}, &text)) {
        double volume; char *start = strstr(text, "Volume:");
        if (start && sscanf(start, "Volume: %lf", &volume) == 1) {
            int result = strstr(text, "[MUTED]") ? 0 : (int)lrint(volume * 100);
            g_free(text); return CLAMP(result, 0, 100);
        }
    }
    g_free(text); text = NULL;
    int result = -1;
    if (audio((const char *const[]){"pactl", "get-sink-volume", "@DEFAULT_SINK@", NULL}, &text)) {
        char *end = strchr(text, '%');
        if (end) { char *start = end; while (start > text && g_ascii_isdigit(start[-1])) --start; result = CLAMP(atoi(start), 0, 100); }
    }
    g_free(text); text = NULL;
    if (result >= 0 && audio((const char *const[]){"pactl", "get-sink-mute", "@DEFAULT_SINK@", NULL}, &text) && strstr(text, "yes")) result = 0;
    g_free(text); return result;
}
static int read_integer(const char *path)
{
    FILE *file = fopen(path, "r"); int value = -1;
    if (file) { if (fscanf(file, "%d", &value) != 1) value = -1; fclose(file); }
    return value;
}
static char *backlight_directory(const char *root)
{
    char *selected = NULL;
    GDir *directory = g_dir_open(root, 0, NULL);
    if (!directory) return NULL;
    const char *name;
    unsigned count = 0;
    while ((name = g_dir_read_name(directory))) {
        char *candidate = g_build_filename(root, name, NULL);
        char *maximum = g_build_filename(candidate, "max_brightness", NULL);
        char *brightness = g_build_filename(candidate, "brightness", NULL);
        gboolean valid = read_integer(maximum) > 0 && read_integer(brightness) >= 0;
        g_free(maximum); g_free(brightness);
        if (valid) {
            ++count;
            if (!selected || !strcmp(name, "fl7703n_backlight")) {
                g_free(selected); selected = candidate; candidate = NULL;
            }
        }
        g_free(candidate);
    }
    g_dir_close(directory);
    /* Preserve the existing panel preference. Other boards must expose one
     * unambiguous backlight, rather than choosing a random display. */
    if (count > 1 && !g_str_has_suffix(selected, "/fl7703n_backlight"))
        g_clear_pointer(&selected, g_free);
    return selected;
}
int sl_backlight_at(const char *class_directory, int percent)
{
    if (percent > 100) return -1;
    char *directory = backlight_directory(class_directory);
    if (!directory) { sl_debug("backlight unavailable: no valid unambiguous device in %s", class_directory); return -1; }
    char *path = g_build_filename(directory, "brightness", NULL);
    char *maximum_path = g_build_filename(directory, "max_brightness", NULL);
    int maximum = read_integer(maximum_path), result = -1;
    g_free(directory); g_free(maximum_path);
    if (maximum <= 0) goto done;
    if (percent >= 0) {
        FILE *file = fopen(path, "w");
        if (!file) { sl_debug("backlight write open failed path=%s errno=%d", path, errno); goto done; }
        int ok = fprintf(file, "%lld\n", ((long long)percent * maximum + 50) / 100) > 0;
        if (fclose(file)) ok = 0;
        result = ok ? percent : -1;
    } else {
        int value = read_integer(path);
        result = value < 0 ? -1 : (int)MIN(100, ((long long)value * 100 + maximum / 2) / maximum);
    }
 done:
    sl_debug("backlight path=%s request_percent=%d max=%d result=%d", path, percent, maximum, result);
    g_free(path); return result;
}
int sl_backlight(int percent)
{
    return sl_backlight_at("/sys/class/backlight", percent);
}
char *sl_wifi_ssid(void)
{
    char *uuid = NULL, *ssid = NULL;
    if (CAPTURE(&uuid, 5, "nmcli", "-g", "GENERAL.CON-UUID", "device", "show", "wlan0")) {
        g_strstrip(uuid);
        char *newline = strchr(uuid, '\n'); if (newline) *newline = 0;
        if (*uuid && strcmp(uuid, "--")) {
            if (!CAPTURE(&ssid, 5, "nmcli", "--escape", "no", "-g", "802-11-wireless.ssid", "connection", "show", "uuid", uuid)) g_clear_pointer(&ssid, g_free);
            if (ssid) { size_t n = strlen(ssid); while (n && (ssid[n-1] == '\r' || ssid[n-1] == '\n')) ssid[--n] = 0; }
        }
    }
    g_free(uuid); return ssid ? ssid : g_strdup("");
}
int sl_wifi_ip(uint8_t ip[4])
{
    char *text = NULL;
    int ok = CAPTURE(&text, 5, "nmcli", "-g", "IP4.ADDRESS", "device", "show", "wlan0");
    if (ok) { g_strstrip(text); char *end = strpbrk(text, "/\r\n"); if (end) *end = 0; ok = inet_pton(AF_INET, text, ip) == 1; }
    g_free(text); return ok;
}
int sl_wifi_connect(const char *ssid, const char *password, uint8_t ip[4])
{
    if (!*ssid || strlen(ssid) > 32) return 0;
    RUN(10, "nmcli", "device", "wifi", "rescan");
    g_usleep(3 * G_USEC_PER_SEC);
    char *profiles = NULL, *uuid = NULL; int rank = 3;
    CAPTURE(&profiles, 5, "nmcli", "--escape", "no", "-t", "-f", "UUID,TYPE,NAME", "connection", "show");
    char **lines = g_strsplit(profiles ? profiles : "", "\n", -1);
    for (size_t i = 0; lines[i]; ++i) {
        char **parts = g_strsplit(lines[i], ":", 3);
        if (g_strv_length(parts) == 3 && (!strcmp(parts[1], "wifi") || !strcmp(parts[1], "802-11-wireless"))) {
            char *saved = NULL;
            if (CAPTURE(&saved, 5, "nmcli", "--escape", "no", "-g", "802-11-wireless.ssid", "connection", "show", "uuid", parts[0])) {
                size_t n = strlen(saved); while (n && (saved[n-1] == '\r' || saved[n-1] == '\n')) saved[--n] = 0;
                int candidate = g_str_has_prefix(parts[2], "shadowlink-wifi-") ? 0 : !strcmp(parts[2], ssid) ? 1 : 2;
                if (!strcmp(saved, ssid) && candidate < rank) { g_free(uuid); uuid = g_strdup(parts[0]); rank = candidate; }
            }
            g_free(saved);
        }
        g_strfreev(parts);
    }
    int ok = 0;
    if (!uuid) {
        uuid = g_uuid_string_random(); char name[64]; snprintf(name, sizeof(name), "shadowlink-wifi-%.8s", uuid);
        if (!RUN(10, "nmcli", "connection", "add", "type", "wifi", "ifname", "wlan0", "con-name", name, "autoconnect", "no", "ssid", ssid, "--", "connection.uuid", uuid)) goto done;
    }
    const char *arguments[64] = {"nmcli", "connection", "modify", "uuid", uuid,
        "connection.autoconnect", "yes", "connection.autoconnect-retries", "0", "connection.autoconnect-priority", "999",
        "connection.permissions", "", "connection.interface-name", "wlan0", "802-11-wireless.ssid", ssid,
        "802-11-wireless.mode", "infrastructure", "802-11-wireless.powersave", "2",
        "ipv4.method", "auto", "ipv4.route-metric", "50", "ipv6.method", "auto", "ipv6.route-metric", "50", NULL};
    if (password && *password) {
        size_t n = 0; while (arguments[n]) ++n;
        arguments[n++] = "wifi-sec.key-mgmt"; arguments[n++] = "wpa-psk";
        arguments[n++] = "wifi-sec.psk"; arguments[n++] = password;
        arguments[n++] = "wifi-sec.psk-flags"; arguments[n++] = "0"; arguments[n] = NULL;
    }
    if (!sl_run(arguments, 10, NULL)) goto done;
    if (!password || !*password) {
        char *security = NULL;
        int queried = CAPTURE(&security, 5, "nmcli", "-g", "802-11-wireless-security.key-mgmt", "connection", "show", "uuid", uuid);
        gboolean remove = security && *g_strstrip(security); g_free(security);
        if (!queried || (remove && !RUN(5, "nmcli", "connection", "modify", "uuid", uuid, "remove", "wifi-sec"))) goto done;
    }
    char *activation_output = NULL;
    int connected = sl_run_with_stderr((const char *const[]){"env", "LC_ALL=C", "nmcli",
        "connection", "up", "uuid", uuid, "ifname", "wlan0", NULL}, 30, &activation_output);
    /* GENERAL.REASON can already be reset to none when nmcli returns. Use the
     * error from this activation instead; never log raw command output. */
    if (!connected && activation_output &&
        strstr(activation_output, "The Wi-Fi network could not be found")) ok = SL_WIFI_NOT_FOUND;
    g_free(activation_output);
    if (!connected) {
        sl_debug("WiFi activation failed: network_not_found=%d", ok == SL_WIFI_NOT_FOUND);
        goto done;
    }
    RUN(10, "nmcli", "connection", "modify", "uuid", uuid,
        "connection.autoconnect", "yes", "connection.autoconnect-retries", "0", "connection.autoconnect-priority", "999",
        "802-11-wireless.powersave", "2", "ipv4.route-metric", "50", "ipv6.route-metric", "50");
    g_strfreev(lines); g_free(profiles); profiles = NULL;
    CAPTURE(&profiles, 5, "nmcli", "--escape", "no", "-t", "-f", "UUID,TYPE,NAME", "connection", "show");
    lines = g_strsplit(profiles ? profiles : "", "\n", -1);
    for (size_t i = 0; lines[i]; ++i) {
        char **parts = g_strsplit(lines[i], ":", 3);
        if (g_strv_length(parts) >= 2 && strcmp(parts[0], uuid) && (!strcmp(parts[1], "wifi") || !strcmp(parts[1], "802-11-wireless")))
            RUN(5, "nmcli", "connection", "modify", "uuid", parts[0], "connection.autoconnect", "no", "connection.autoconnect-priority", "-100", "ipv4.route-metric", "600", "ipv6.route-metric", "600");
        g_strfreev(parts);
    }
    for (int i = 0; i < 5; ++i) { if (sl_wifi_ip(ip)) { ok = 1; break; } if (i < 4) g_usleep(3 * G_USEC_PER_SEC); }
    if (ok) ok = RUN(60, "docker", "restart", "homeassistant");
 done:
    g_strfreev(lines); g_free(profiles); g_free(uuid); return ok;
}
static void mounted_usage(json_object *device, GHashTable *seen, uint64_t *used)
{
    json_object *mounts = NULL, *children = NULL;
    json_object_object_get_ex(device, "mountpoints", &mounts);
    gboolean single = json_object_is_type(mounts, json_type_string);
    size_t count = single ? 1 : json_object_is_type(mounts, json_type_array) ? json_object_array_length(mounts) : 0;
    for (size_t i = 0; i < count; ++i) {
        json_object *mount = single ? mounts : json_object_array_get_idx(mounts, i);
        if (!json_object_is_type(mount, json_type_string)) continue;
        const char *path = json_object_get_string(mount); struct stat st; struct statvfs fs;
        if (*path != '/' || stat(path, &st) || statvfs(path, &fs)) continue;
        char id[64]; snprintf(id, sizeof(id), "%llu", (unsigned long long)st.st_dev);
        if (g_hash_table_contains(seen, id)) continue;
        g_hash_table_add(seen, g_strdup(id)); *used += (fs.f_blocks - fs.f_bfree) * fs.f_frsize;
    }
    json_object_object_get_ex(device, "children", &children);
    count = json_object_is_type(children, json_type_array) ? json_object_array_length(children) : 0;
    for (size_t i = 0; i < count; ++i) mounted_usage(json_object_array_get_idx(children, i), seen, used);
}
void sl_storage(uint64_t usage[4])
{
    memset(usage, 0, 4 * sizeof(*usage)); char *text = NULL;
    if (!CAPTURE(&text, 5, "lsblk", "--json", "--bytes", "--output", "NAME,PATH,TYPE,SIZE,MOUNTPOINTS")) { g_free(text); return; }
    json_object *data = sl_json_parse(text, strlen(text)), *disks = NULL; g_free(text);
    if (!data) return;
    json_object_object_get_ex(data, "blockdevices", &disks);
    GHashTable *seen[2] = {g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL), g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL)};
    size_t n = json_object_is_type(disks, json_type_array) ? json_object_array_length(disks) : 0;
    for (size_t i = 0; i < n; ++i) {
        json_object *disk = json_object_array_get_idx(disks, i), *size = NULL;
        if (strcmp(sl_string(disk, "type"), "disk")) continue;
        const char *name = sl_string(disk, "name");
        int kind = g_str_has_prefix(name, "mmcblk") ? 0 : g_str_has_prefix(name, "nvme") ? 1 : -1;
        if (kind < 0) continue;
        json_object_object_get_ex(disk, "size", &size); usage[kind * 2 + 1] += json_object_get_uint64(size);
        mounted_usage(disk, seen[kind], &usage[kind * 2]);
    }
    g_hash_table_destroy(seen[0]); g_hash_table_destroy(seen[1]); json_object_put(data);
}
gboolean sl_reset_in_progress(void)
{
    const char *path = g_getenv("CLAWSTAGE_RESET_MARKER");
    if (!path || !*path) path = "/home/pi/clawstage/.factory-reset-in-progress";
    struct stat st;
    return lstat(path, &st) == 0 || errno != ENOENT;
}
int sl_reset_lock_held(int fd)
{
    const char *path = g_getenv("CLAWSTAGE_MAINTENANCE_LOCK");
    if (!path || !*path) path = "/run/lock/clawstage-maintenance.lock";
    struct stat inherited, named;
    if (fd < 0 || fstat(fd, &inherited) || lstat(path, &named) || !S_ISREG(named.st_mode) ||
        named.st_uid != geteuid() || (named.st_mode & 0022) ||
        inherited.st_dev != named.st_dev || inherited.st_ino != named.st_ino) return 0;
    int probe = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (probe < 0) return 0;
    int occupied = flock(probe, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK;
    close(probe);
    /* A separately opened descriptor must see the lock, while the descriptor
     * inherited from the lifecycle script must already own it. */
    return occupied && flock(fd, LOCK_EX | LOCK_NB) == 0;
}
static int unbind_step_done(json_object *journal, const char *step)
{
    json_object *value = NULL;
    return journal && json_object_object_get_ex(journal, step, &value) &&
        json_object_is_type(value, json_type_boolean) && json_object_get_boolean(value);
}
static int save_unbind_step(const char *path, json_object *journal, const char *step, int success)
{
    if (!success) return 0;
    json_object_object_add(journal, step, json_object_new_boolean(TRUE));
    return sl_json_write(path, journal);
}
static char *reset_boot_id(void)
{
    char *id = NULL;
    if (!g_file_get_contents("/proc/sys/kernel/random/boot_id", &id, NULL, NULL)) return NULL;
    g_strstrip(id);
    if (!g_uuid_string_is_valid(id)) { g_free(id); return NULL; }
    return id;
}
static int reset_sent_this_boot(json_object *journal)
{
    char *id = reset_boot_id();
    int sent = id && !strcmp(id, sl_string(journal, "reset_sent_boot_id"));
    g_free(id); return sent;
}
static int notify_unbind(const char *path, json_object *journal)
{
    if (reset_sent_this_boot(journal)) return 1;
    char *id = reset_boot_id();
    int ok = id && sl_notify_unbind_reset();
    if (ok) {
        /* Retain intent until rebind. A relay write cannot prove that RESET
         * survived renderer/host restart; a new boot must send it again. */
        json_object_object_add(journal, "reset_sent_boot_id", json_object_new_string(id));
        ok = sl_json_write(path, journal);
    }
    g_free(id); return ok;
}
static int retire_unbind(const char *path, const char *release)
{
    if (unlink(path)) return 0;
    int fd = open(release, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int ok = fd >= 0 && fsync(fd) == 0;
    if (fd >= 0) close(fd);
    return ok;
}
/* Caller owns the exclusive maintenance lock and has committed a new binding. */
int sl_retire_completed_unbind(sl_runtime *r)
{
    if (!sl_unbind_pending(r)) return 1;
    json_object *journal = sl_unbind_journal(r);
    int completed = !strcmp(sl_string(journal, "action"), "unbind") &&
        unbind_step_done(journal, "confirmed") && unbind_step_done(journal, "cleanup_complete");
    json_object_put(journal);
    if (!completed) return 0;
    char *path = g_build_filename(r->release, ".device-unbind.json", NULL);
    int ok = retire_unbind(path, r->release);
    g_free(path); return ok;
}
static void queue_unbind_notification(void)
{
    if (!RUN(5, "systemctl", "start", "--no-block", "clawstage-shadowlink-reset.service"))
        sl_log("Shadowlink reset: cannot queue notification recovery; intent retained for boot\n");
}
/* Boot recovery owns service startup; in-process Status Recover preserves BLE. */
static int recover_unbound_services(sl_runtime *r)
{
    /* Cleanup is already durable. Notification retries must not disconnect a
     * phone which is trying to bind the freshly reset device. */
    char *compose = g_build_filename(r->release, "homeassistant", "stage-stack", "compose.release.yaml", NULL);
    int ok = RUN(60, "docker", "compose", "-f", compose, "up", "-d", "--no-build", "--pull", "never");
    g_free(compose);
    return ok;
}
/* Live retries can resume steps; boot recovery reruns incomplete cleanup. */
static int unbind_locked(sl_runtime *r, gboolean preserve_session, gboolean resume_only, gboolean recovery)
{
    if (sl_reset_in_progress()) { return 0; }
    char *journal_path = g_build_filename(r->release, ".device-unbind.json", NULL);
    json_object *journal = sl_unbind_journal(r);
    if (!strcmp(sl_string(journal, "action"), "unbind") &&
        unbind_step_done(journal, "confirmed") && unbind_step_done(journal, "cleanup_complete")) {
        json_object *config = sl_json_read(r->config);
        if (!config) { json_object_put(journal); g_free(journal_path); return 0; }
        int unbound = sl_commissioning_status(config) == 1;
        json_object_put(config);
        if (!unbound && sl_unbind_identity_matches(r, journal)) {
            /* An abnormal/partial config with no new identity is not evidence
             * of rebind. Preserve intent, but never reset ambiguous state. */
            json_object_put(journal); g_free(journal_path); return 0;
        }
        /* Never replay an old RESET against a new (even partial) binding. */
        if (unbound && !reset_sent_this_boot(journal) && recovery && !recover_unbound_services(r)) {
            json_object_put(journal); g_free(journal_path); return 0;
        }
        int notified = unbound ? notify_unbind(journal_path, journal) : retire_unbind(journal_path, r->release);
        if (!notified && !recovery) queue_unbind_notification();
        json_object_put(journal); journal = NULL;
        if (resume_only || unbound || !notified) {
            g_free(journal_path);
            return recovery ? notified : (unbound || notified);
        }
        /* An explicit unbind of a new owner starts a new authorization below. */
    }
    json_object_put(journal);
    if ((resume_only && !sl_unbind_pending(r)) ||
        !(preserve_session ? sl_prepare_recover(r) : sl_authorize_unbind(r, "unbind"))) {
        g_free(journal_path); return 0;
    }
    journal = sl_unbind_journal(r);
    if (!journal) { g_free(journal_path); return 0; }
    if (recovery) {
        /* A pending cleanup starts over after restart, regardless of recorded
         * partial progress. Keep identity and cloud request policy intact. */
        const char *steps[] = {"config", "clear", "purge", "reset_sent_boot_id"};
        for (size_t i = 0; i < G_N_ELEMENTS(steps); ++i) json_object_object_del(journal, steps[i]);
        if (!sl_json_write(journal_path, journal)) {
            json_object_put(journal); g_free(journal_path); return 0;
        }
    }
    if (recovery && !RUN(30, "systemctl", "stop", "shadowlink-ble-server.service")) {
        json_object_put(journal); g_free(journal_path); return 0;
    }
    r->commissioning_response_sent = FALSE;
    if (!preserve_session) {
        sl_session_reset(&r->session); r->ota_authenticated = FALSE; r->last_status = -1; r->recovered = 0;
    }
    g_clear_pointer(&r->pending_userinfo, g_free);
    r->advertisement_changed = TRUE;
    int config_ok = unbind_step_done(journal, "config") ||
        save_unbind_step(journal_path, journal, "config", sl_config_reset(r) || sl_config_reset(r));
    if (config_ok) r->invalidate_peers = TRUE;
    char *clear = g_build_filename(r->release, "openclaw", "hoorii_claw", "script", "clear.sh", NULL);
    const char *clear_command[] = {"runuser", "-u", "pi", "--", clear, NULL};
    int clear_ok = unbind_step_done(journal, "clear") ||
        save_unbind_step(journal_path, journal, "clear", sl_run(clear_command, 300, NULL) || sl_run(clear_command, 300, NULL));
    char *stack = g_build_filename(r->release, "homeassistant", "stage-stack", NULL);
    char *script = g_build_filename(stack, "install.sh", NULL);
    char *data = g_build_filename(r->release, "user", ".hoorii", "stage-stack", NULL);
    const char *purge[] = {"env", "--chdir", stack, "HOME=/home/pi", "SUDO_USER=pi",
        script, "--purge", "all", "-f", "--no-pull", "--system-dir", stack, "--data-dir", data, NULL};
    int purge_ok = unbind_step_done(journal, "purge") ||
        save_unbind_step(journal_path, journal, "purge", sl_run(purge, 120, NULL) || sl_run(purge, 120, NULL));
    if (recovery && purge_ok) {
        char *compose = g_build_filename(stack, "compose.release.yaml", NULL);
        purge_ok = RUN(60, "docker", "compose", "-f", compose, "up", "-d", "--no-build", "--pull", "never");
        g_free(compose);
    }
    int core_ok = config_ok && clear_ok && purge_ok &&
        save_unbind_step(journal_path, journal, "cleanup_complete", 1);
    int ws_ok = core_ok && notify_unbind(journal_path, journal);
    sl_debug("BLE unbind config=%d clear=%d purge=%d RESET=%d", config_ok, clear_ok, purge_ok, ws_ok);
    g_free(clear); g_free(stack); g_free(script); g_free(data);
    if (core_ok && !ws_ok && !recovery) queue_unbind_notification();
    int ok = core_ok && (!recovery || ws_ok);
    json_object_put(journal); g_free(journal_path);
    return ok;
}
static int unbind_impl(sl_runtime *r, gboolean preserve_session, gboolean resume_only)
{
    int lock = sl_binding_lock();
    if (lock < 0) return 0;
    int ok = unbind_locked(r, preserve_session, resume_only, FALSE);
    close(lock);
    return ok;
}
int sl_unbind(sl_runtime *r, gboolean preserve_session)
{
    return unbind_impl(r, preserve_session, FALSE);
}
int sl_resume_unbind(sl_runtime *r)
{
    return unbind_impl(r, TRUE, TRUE);
}
int sl_recover_unbind(sl_runtime *r)
{
    /* Only the active release can consume a durable reset request. */
    const char *state = g_getenv("CLAWSTAGE_STATE_DIR");
    char *current = g_build_filename(state && *state ? state : "/home/pi/clawstage", "current", NULL);
    char *active = realpath(current, NULL), *release = realpath(r->release, NULL);
    int valid = active && release && !strcmp(active, release);
    free(active); free(release); g_free(current);
    if (!valid) return 0;
    int lock = sl_binding_lock();
    if (lock < 0) return 0;
    int ok = 0;
    json_object *journal = NULL;
    if (!sl_unbind_pending(r)) { ok = 1; goto done; }
    journal = sl_unbind_journal(r);
    if (!journal || strcmp(sl_string(journal, "action"), "unbind") || sl_reset_in_progress()) goto done;
    /* OTA reservations survive process restarts and must be checked under the
     * same host lock used by the update controller to accept a new reservation. */
    json_object *status = sl_update_call("get_status", NULL, NULL, NULL), *busy = NULL;
    gboolean idle = status && json_object_object_get_ex(status, "maintenance_busy", &busy) &&
        json_object_is_type(busy, json_type_boolean) && !json_object_get_boolean(busy);
    json_object_put(status);
    if (!idle) goto done;
    if (unbind_step_done(journal, "confirmed") && unbind_step_done(journal, "cleanup_complete")) {
        ok = unbind_locked(r, FALSE, TRUE, TRUE);
        goto done;
    }
    if (!sl_unbind_identity_matches(r, journal)) goto done;
    if (!unbind_step_done(journal, "confirmed") && !unbind_step_done(journal, "cloud_request_attempted") &&
        !unbind_step_done(journal, "local_only")) {
        g_print("Shadowlink reset recovery: starting Gateway for one best-effort cloud unbind request\n");
        char *startup = g_build_filename(r->release, "openclaw", "hoorii_claw", "script", "startup.sh", NULL);
        char *home = g_strdup_printf("HOME=%s/user", r->release);
        /* Reuse the component's startup/readiness contract, including its real
         * user environment. No installer-side Gateway configuration repair. */
        int ready = RUN(420, "runuser", "-u", "pi", "--", "env", home, "USER=pi", "LOGNAME=pi",
            "HOORII_GATEWAY_REAL_USER=pi", "HOORII_GATEWAY_REAL_HOME=/home/pi", "bash", startup);
        g_free(startup); g_free(home);
        if (!ready) sl_log("Gateway startup failed; still attempting cloud request and local cleanup\n");
    } else {
        g_print("Shadowlink reset recovery: cloud request already handled; continuing local cleanup\n");
    }
    ok = unbind_locked(r, FALSE, TRUE, TRUE);
 done:
    json_object_put(journal);
    close(lock);
    return ok;
}
int sl_reset_services(sl_runtime *r, gboolean from_shadowlink, gboolean *accepted)
{
    char *script = g_build_filename(r->release, "factory-reset.sh", NULL);
    const char *command[] = {"/bin/bash", script, "--release-dir", r->release, from_shadowlink ? "--from-shadowlink" : NULL, NULL};
    gboolean started = FALSE;
    int ok = sl_run_logged(command, 1800, "CLAWSTAGE_RESET_ACCEPTED", &started);
    if (accepted) *accepted = started;
    if (!started) sl_log("[FACTORY-RESET-ERROR] Reset was not accepted; see preflight/lifecycle errors above\n");
    g_free(script);
    return ok && started;
}

void sl_privacy_states(const char *path, int states[3])
{
    states[0] = states[1] = states[2] = -1;
    json_object *state = sl_json_read(path), *value = NULL;
    if (state && json_object_object_get_ex(state, "mute_stat", &value) && json_object_is_type(value, json_type_int)) {
        int n = json_object_get_int(value); if (n == 0 || n == 1) states[0] = n;
    }
    json_object *version = NULL, *camera = NULL, *microphone = NULL;
    if (state && json_object_object_get_ex(state, "schema_version", &version) &&
        json_object_is_type(version, json_type_int) && json_object_get_int(version) == 2 &&
        json_object_object_get_ex(state, "camera_enabled", &camera) && json_object_is_type(camera, json_type_boolean) &&
        json_object_object_get_ex(state, "microphone_enabled", &microphone) && json_object_is_type(microphone, json_type_boolean)) {
        states[1] = json_object_get_boolean(camera); states[2] = json_object_get_boolean(microphone);
    }
    json_object_put(state);
}
