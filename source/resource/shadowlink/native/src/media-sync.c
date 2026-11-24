#include "runtime.h"
#include <string.h>

char *sl_media_binding(sl_runtime *r, const char *hub)
{
    if (r->session.state != 2 || !hub || !*hub) return NULL;
    GChecksum *hash = g_checksum_new(G_CHECKSUM_SHA256);
    const char label[] = "hoorii-media-read-v2";
    g_checksum_update(hash, (const guchar *)label, sizeof(label));
    const uint8_t *fields[] = {(const uint8_t *)hub, r->session.transaction, r->session.initiator_id,
        r->responder_id, r->session.initiator_public, r->session.ephemeral_public};
    size_t sizes[] = {strlen(hub), 16, 16, 16, 65, 65};
    for (unsigned i = 0; i < G_N_ELEMENTS(fields); ++i) {
        uint32_t n = GUINT32_TO_BE((uint32_t)sizes[i]);
        g_checksum_update(hash, (const guchar *)&n, 4);
        g_checksum_update(hash, fields[i], (gssize)sizes[i]);
    }
    char *result = g_strdup(g_checksum_get_string(hash)); g_checksum_free(hash); return result;
}
static json_object *section(json_object *object, const char *key)
{
    json_object *value = NULL;
    return object && json_object_object_get_ex(object, key, &value) && json_object_is_type(value, json_type_object) ? value : NULL;
}
static json_object *read_at(const char *root, const char *name)
{
    char *path = g_build_filename(root, name, NULL); json_object *value = sl_json_read(path); g_free(path); return value;
}
json_object *sl_media_read(sl_runtime *r, json_object *request, const char *root)
{
    json_object *version = NULL, *unexpected = NULL;
    const char *id = sl_string(request, "authorizationId");
    if (strcmp(sl_string(request, "operation"), "read-media") ||
        !json_object_object_get_ex(request, "protocolVersion", &version) || !json_object_is_type(version, json_type_int) ||
        json_object_get_int(version) != 2 || json_object_object_get_ex(request, "hubToken", &unexpected) || strlen(id) != 43) return NULL;
    for (const char *c = id; *c; ++c) if (!g_ascii_isalnum(*c) && *c != '-' && *c != '_') return NULL;
    json_object *config = sl_json_read(r->config), *ui = section(config, "userinfo"), *owner = NULL;
    json_object *profile = NULL, *after = NULL, *after_owner = NULL, *result = NULL;
    char *binding = NULL, *scope = NULL, *hash = NULL;
    const char *hub = sl_string(ui, "hub_id"), *user = sl_string(ui, "union_id");
    if (!*hub || !*user || !*sl_string(ui, "hub_token") || strcmp(hub, sl_string(request, "hubId"))) goto done;
    scope = g_strconcat(user, ":", hub, NULL); hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, scope, -1);
    owner = read_at(root, "byok-owner.json");
    if (strcmp(sl_string(owner, "scope"), hash)) goto done;
    binding = sl_media_binding(r, hub);
    if (!binding || !sl_media_consume(ui, id, binding)) goto done;
    profile = read_at(root, "byok-profile.json"); after_owner = read_at(root, "byok-owner.json"); after = sl_json_read(r->config);
    if (!profile || !json_object_equal(ui, section(after, "userinfo")) || strcmp(sl_string(after_owner, "scope"), hash)) goto done;
    result = json_object_new_object();
    json_object_object_add(result, "hubId", json_object_new_string(hub));
    json_object_object_add(result, "authorizationId", json_object_new_string(id));
    json_object *updated = NULL;
    json_object_object_get_ex(profile, "updatedAt", &updated);
    json_object_object_add(result, "updatedAt", json_object_new_int64(updated ? json_object_get_int64(updated) : 0));
    const char *names[] = {"asr", "tts"};
    const char *fields[][6] = {{"vendor", "apiKey", "language", "model", "keyterm", NULL}, {"vendor", "apiKey", "modelId", NULL, NULL, NULL}};
    for (unsigned i = 0; i < 2; ++i) {
        json_object *source = NULL;
        if (!json_object_object_get_ex(profile, names[i], &source) || !json_object_is_type(source, json_type_object)) continue;
        json_object *filtered = json_object_new_object();
        for (unsigned j = 0; fields[i][j]; ++j) {
            json_object *value = NULL;
            if (json_object_object_get_ex(source, fields[i][j], &value) && json_object_is_type(value, json_type_string))
                json_object_object_add(filtered, fields[i][j], json_object_get(value));
        }
        json_object_object_add(result, names[i], filtered);
    }
 done:
    json_object_put(config); json_object_put(owner); json_object_put(profile); json_object_put(after); json_object_put(after_owner);
    g_free(binding); g_free(scope); g_free(hash); return result;
}
