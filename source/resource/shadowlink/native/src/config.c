#define _GNU_SOURCE
#include "runtime.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

const char *sl_string(json_object *object, const char *key)
{
    json_object *value = NULL;
    if (!object || !json_object_object_get_ex(object, key, &value) ||
        !json_object_is_type(value, json_type_string)) return "";
    return json_object_get_string(value);
}
json_object *sl_section(json_object *object, const char *key)
{
    json_object *value = NULL;
    if (!json_object_object_get_ex(object, key, &value) ||
        !json_object_is_type(value, json_type_object)) {
        value = json_object_new_object();
        json_object_object_add(object, key, value);
    }
    return value;
}
json_object *sl_commissioning(json_object *config)
{
    json_object *section = NULL;
    if (json_object_object_get_ex(config, "commissioning", &section) &&
        json_object_is_type(section, json_type_object)) return section;
    return config;
}
json_object *sl_json_parse(const char *data, size_t length)
{
    if (length > 1024 * 1024 || !g_utf8_validate(data, (gssize)length, NULL)) {
        sl_debug("JSON rejected: exceeds 1 MiB or contains invalid UTF-8");
        return NULL;
    }
    json_tokener *tokener = json_tokener_new();
    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT);
    json_object *object = json_tokener_parse_ex(tokener, data, (int)length);
    if (json_tokener_get_error(tokener) != json_tokener_success) {
        sl_debug("JSON parse failed: %s (content omitted)", json_tokener_error_desc(json_tokener_get_error(tokener)));
        json_object_put(object); object = NULL;
    }
    json_tokener_free(tokener);
    return object;
}
json_object *sl_json_read(const char *path)
{
    char *data = NULL;
    gsize length = 0;
    GError *error = NULL;
    if (!g_file_get_contents(path, &data, &length, &error)) {
        sl_debug("Cannot read JSON file=%s reason=%s", path, error ? error->message : "unknown");
        g_clear_error(&error);
        return NULL;
    }
    json_object *object = sl_json_parse(data, length);
    g_free(data);
    if (!json_object_is_type(object, json_type_object)) {
        sl_debug("JSON file=%s is invalid or its root is not an object", path);
        json_object_put(object); return NULL;
    }
    return object;
}
int sl_json_write(const char *path, json_object *object)
{
    char *temporary = g_strconcat(path, ".XXXXXX", NULL);
    char *directory = g_path_get_dirname(path);
    struct stat previous;
    gboolean existed = stat(path, &previous) == 0;
    int fd = g_mkstemp_full(temporary, O_RDWR | O_CLOEXEC, 0600), ok = 0, saved_errno;
    if (fd < 0) goto done;
    const char *text = json_object_to_json_string_ext(object, JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_NOSLASHESCAPE);
    size_t length = strlen(text), written = 0;
    while (written < length) {
        ssize_t n = write(fd, text + written, length - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto done;
        written += (size_t)n;
    }
    if (write(fd, "\n", 1) != 1) goto done;
    if (existed && (fchown(fd, previous.st_uid, previous.st_gid) || fchmod(fd, previous.st_mode & 0777))) goto done;
    if (fsync(fd) || rename(temporary, path)) goto done;
    int parent = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (parent < 0) goto done;
    ok = fsync(parent) == 0;
    close(parent);
 done:
    saved_errno = errno;
    if (fd >= 0) close(fd);
    if (!ok) unlink(temporary);
    g_free(temporary); g_free(directory);
    if (!ok) {
        sl_debug("Cannot persist JSON file=%s reason=%s", path, strerror(saved_errno));
        errno = saved_errno;
    }
    return ok;
}
int sl_unhex(const char *text, uint8_t *out, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        while (g_ascii_isspace(*text)) ++text;
        int a = g_ascii_xdigit_value(*text);
        if (a < 0) return 0;
        ++text;
        int b = g_ascii_xdigit_value(*text);
        if (b < 0) return 0;
        ++text;
        out[i] = (uint8_t)((a << 4) | b);
    }
    while (g_ascii_isspace(*text)) ++text;
    return !*text;
}
char *sl_hex(const uint8_t *bytes, size_t length)
{
    char *result = g_malloc(length * 2 + 1);
    for (size_t i = 0; i < length; ++i) snprintf(result + i * 2, 3, "%02x", bytes[i]);
    result[length * 2] = 0;
    return result;
}
static int number(json_object *object, const char *key, int fallback, int string_base)
{
    json_object *value = NULL;
    if (!json_object_object_get_ex(object, key, &value)) return fallback;
    if (json_object_is_type(value, json_type_int)) return json_object_get_int(value);
    if (!json_object_is_type(value, json_type_string)) return fallback;
    const char *text = json_object_get_string(value);
    char *end = NULL;
    long result = strtol(text, &end, string_base);
    return end && end != text && !*end && result <= INT_MAX && result >= INT_MIN ? (int)result : fallback;
}
static int responder_id(uint8_t id[16])
{
    char *serial = NULL;
    memset(id, 0, 16); id[0] = 1;
    if (g_file_get_contents("/sys/firmware/devicetree/base/serial-number", &serial, NULL, NULL)) {
        g_strstrip(serial);
        int ok = sl_unhex(serial, id + 8, 8);
        g_free(serial);
        if (ok) return 1;
    }
    FILE *file = fopen("/proc/cpuinfo", "r");
    if (!file) return 0;
    char line[512]; int ok = 0;
    while (fgets(line, sizeof(line), file)) {
        if (!strncmp(line, "Serial", 6)) {
            char *colon = strchr(line, ':');
            if (colon) ok = sl_unhex(g_strstrip(colon + 1), id + 8, 8);
            break;
        }
    }
    fclose(file);
    return ok;
}
int sl_identity_load(sl_runtime *r)
{
    json_object *config = sl_json_read(r->config);
    if (!config) return 0;
    json_object *c = sl_commissioning(config);
    int passcode = number(c, "passcode", 0, g_str_has_prefix(sl_string(c, "passcode"), "0x") ? 16 : 10);
    int discriminator = number(c, "discriminator", -1, 16);
    char qr[58];
    const char *failure = NULL;
    if (!(r->responder_id[0] == 1 || responder_id(r->responder_id))) failure = "device serial unavailable or invalid";
    else if (!sl_unhex(sl_string(c, "long_term_private_key"), r->private_key, 32)) failure = "long_term_private_key missing or not 32-byte hex";
    else if (!sl_public_key(r->private_key, r->public_key)) failure = "cannot derive public key from private key";
    else if (discriminator < 0 || discriminator > 4095) failure = "discriminator missing or outside 0..4095";
    else if (passcode <= 0) failure = "passcode missing or invalid";
    else if (!sl_onboarding(0xfff1, 0x8000, (uint16_t)discriminator, (uint32_t)passcode, r->responder_id, qr, sizeof(qr)))
        failure = "cannot construct onboarding identity from configured fields";
    else if (strcmp(sl_string(c, "qrcode"), qr)) failure = "qrcode does not match established device identity";
    int ok = failure == NULL;
    if (failure) sl_debug("Identity validation failed file=%s reason=%s", r->config, failure);
    r->passcode = (uint32_t)passcode;
    if (ok) r->discriminator = (uint16_t)discriminator;
    json_object_put(config);
    return ok;
}
int sl_identity_init(sl_runtime *r, const char *version, gboolean allow_create)
{
    json_object *config = sl_json_read(r->config);
    if (!config) {
        struct stat existing;
        /* A dangling identity symlink is damaged state, not a new device. */
        if (!allow_create || !lstat(r->config, &existing) || errno != ENOENT ||
            !(r->responder_id[0] == 1 || responder_id(r->responder_id))) return 0;
        uint8_t random[8]; char qr[58];
        if (!sl_keygen(r->private_key, r->public_key)) return 0;
        uint32_t passcode; uint16_t discriminator;
        do {
            if (RAND_bytes(random, sizeof(random)) != 1) return 0;
            discriminator = (uint16_t)(((unsigned)random[0] << 8 | random[1]) & 4095);
            memcpy(&passcode, random + 4, 4); passcode = 1 + passcode % 99999998;
        } while (!sl_onboarding(0xfff1, 0x8000, discriminator, passcode, r->responder_id, qr, sizeof(qr)));
        config = json_object_new_object();
        json_object *c = sl_section(config, "commissioning");
        char *key = sl_hex(r->private_key, 32);
        json_object_object_add(c, "long_term_private_key", json_object_new_string(key)); g_free(key);
        json_object_object_add(c, "qrcode", json_object_new_string(qr));
        json_object_object_add(c, "vendor_id", json_object_new_string("0xfff1"));
        json_object_object_add(c, "product_id", json_object_new_string("0x8000"));
        json_object_object_add(c, "discriminator", json_object_new_int(discriminator));
        json_object_object_add(c, "passcode", json_object_new_int64(passcode));
        json_object_object_add(c, "commissioning_flag", json_object_new_int(0));
        json_object_object_add(c, "kpersistent", json_object_new_string(""));
    } else {
        if (!sl_identity_load(r)) { json_object_put(config); return 0; }
        if (sl_commissioning(config) == config) {
            static const char *keys[] = {"qrcode", "long_term_private_key", "vendor_id", "product_id", "discriminator", "passcode", "commissioning_flag", "kpersistent"};
            json_object *c = json_object_new_object();
            for (size_t i = 0; i < G_N_ELEMENTS(keys); ++i) {
                json_object *value;
                if (json_object_object_get_ex(config, keys[i], &value)) {
                    json_object_object_add(c, keys[i], json_object_get(value));
                    json_object_object_del(config, keys[i]);
                }
            }
            json_object_object_add(config, "commissioning", c);
        }
    }
    const char *sections[] = {"environment", "userinfo"};
    for (size_t i = 0; i < G_N_ELEMENTS(sections); ++i) {
        json_object *existing = NULL;
        if (json_object_object_get_ex(config, sections[i], &existing) &&
            !json_object_is_type(existing, json_type_object)) { json_object_put(config); return 0; }
    }
    json_object *ui = sl_section(config, "userinfo"), *environment = sl_section(config, "environment");
    const char *keys[] = {"hub_token", "union_id", "hub_id", "ha_token"};
    for (size_t i = 0; i < G_N_ELEMENTS(keys); ++i) {
        json_object *value;
        if (!json_object_object_get_ex(ui, keys[i], &value)) json_object_object_add(ui, keys[i], json_object_new_string(""));
    }
    json_object_object_add(environment, "stage_version", json_object_new_string(version));
    json_object *old_env;
    if (!json_object_object_get_ex(environment, "stage_env", &old_env)) json_object_object_add(environment, "stage_env", json_object_new_string("prod"));
    char *directory = g_path_get_dirname(r->config);
    int ok = g_mkdir_with_parents(directory, 0755) == 0 && sl_json_write(r->config, config);
    g_free(directory);
    if (ok) {
        char *path = g_build_filename(r->release, "stage-ui", "dist", "dev-payload.json", NULL);
        if (g_file_test(path, G_FILE_TEST_EXISTS)) {
            json_object *payload = sl_json_read(path);
            ok = payload != NULL;
            if (payload) {
                json_object_object_add(payload, "Payload", json_object_new_string(sl_string(sl_commissioning(config), "qrcode")));
                ok = sl_json_write(path, payload); json_object_put(payload);
            }
        }
        g_free(path);
    }
    json_object_put(config);
    return ok;
}
static int commissioning_status(json_object *config, gboolean require_ha)
{
    if (!config) return 2;
    json_object *c = sl_commissioning(config), *ui = NULL;
    json_object_object_get_ex(config, "userinfo", &ui);
    int flag = number(c, "commissioning_flag", number(c, "commission_flag", -1, 10), 10);
    gboolean complete = TRUE, empty = TRUE;
    const char *keys[] = {"hub_token", "union_id", "hub_id", "ha_token"};
    for (size_t i = 0; i < G_N_ELEMENTS(keys) - (require_ha ? 0 : 1); ++i) {
        json_object *value = NULL;
        if (!ui || !json_object_object_get_ex(ui, keys[i], &value) || !json_object_is_type(value, json_type_string)) { complete = empty = FALSE; break; }
        char *text = g_strdup(sl_string(ui, keys[i])); g_strstrip(text);
        if (*text) empty = FALSE; else complete = FALSE;
        g_free(text);
    }
    uint8_t persistent[32];
    json_object *persistent_value = NULL;
    if (json_object_object_get_ex(c, "kpersistent", &persistent_value) && !json_object_is_type(persistent_value, json_type_string)) return 2;
    char *text = g_strdup(sl_string(c, "kpersistent")); g_strstrip(text);
    int result = flag == 1 && complete && sl_unhex(text, persistent, 32) ? 0 : flag == 0 && empty && !*text ? 1 : 2;
    OPENSSL_cleanse(persistent, sizeof(persistent)); g_free(text);
    return result;
}
int sl_commissioning_status(json_object *config)
{
    return commissioning_status(config, TRUE);
}
gboolean sl_binding_committed(json_object *config)
{
    return commissioning_status(config, FALSE) == 0;
}

int sl_config_reset(sl_runtime *r)
{
    json_object *config = sl_json_read(r->config);
    if (!config) return 0;
    json_object *c = sl_commissioning(config), *ui = json_object_new_object();
    /* Older releases used commission_flag or kept binding state at the root.
     * Retire both aliases so no legacy reader can observe a stale bound state. */
    json_object_object_del(c, "commission_flag");
    if (c != config) {
        json_object_object_del(config, "commission_flag");
        json_object_object_del(config, "commissioning_flag");
        json_object_object_del(config, "kpersistent");
    }
    json_object_object_add(c, "commissioning_flag", json_object_new_int(0));
    json_object_object_add(c, "kpersistent", json_object_new_string(""));
    const char *keys[] = {"hub_token", "union_id", "hub_id", "ha_token"};
    for (size_t i = 0; i < G_N_ELEMENTS(keys); ++i) json_object_object_add(ui, keys[i], json_object_new_string(""));
    json_object_object_add(config, "userinfo", ui);
    int ok = sl_json_write(r->config, config); json_object_put(config);
    return ok;
}
