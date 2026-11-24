#include "runtime.h"
#include <curl/curl.h>
#include <string.h>

static size_t receive(void *data, size_t size, size_t count, void *user)
{
    GString *buffer = user;
    if (size && count > (8192 - buffer->len) / size) return 0;
    size_t length = size * count; g_string_append_len(buffer, data, (gssize)length); return length;
}
static gboolean yes(json_object *object, const char *key)
{
    json_object *value = NULL;
    return object && json_object_object_get_ex(object, key, &value) &&
        json_object_is_type(value, json_type_boolean) && json_object_get_boolean(value);
}
int sl_media_consume(json_object *userinfo, const char *id, const char *binding)
{
    const char *base = g_getenv("HOORII_API_BASE_URL");
    if (!base || !*base) base = "https://api.studio.hoorii.io";
    GUri *uri = g_uri_parse(base, G_URI_FLAGS_NONE, NULL);
    if (!uri) return 0;
    gboolean valid = !g_strcmp0(g_uri_get_scheme(uri), "https") && g_uri_get_host(uri) &&
        !g_uri_get_userinfo(uri) && !g_uri_get_query(uri) && !g_uri_get_fragment(uri);
    g_uri_unref(uri);
    const char *hub = sl_string(userinfo, "hub_id"), *token = sl_string(userinfo, "hub_token");
    if (!valid || !*hub || !*token || strpbrk(hub, "\r\n") || strpbrk(token, "\r\n")) return 0;
    CURL *curl = curl_easy_init(); if (!curl) return 0;
    char *normalized = g_strdup(base); size_t n = strlen(normalized);
    while (n && normalized[n - 1] == '/') normalized[--n] = 0;
    char *url = g_strconcat(normalized, "/studio/hub/byok/media-sync/consume", NULL); g_free(normalized);
    json_object *body = json_object_new_object();
    json_object_object_add(body, "authorizationId", json_object_new_string(id));
    json_object_object_add(body, "channelBinding", json_object_new_string(binding));
    struct curl_slist *headers = curl_slist_append(NULL, "Content-Type: application/json");
    char *header = g_strconcat("hoorii-hub-id: ", hub, NULL); headers = curl_slist_append(headers, header); g_free(header);
    header = g_strconcat("hoorii-hub-token: ", token, NULL); headers = curl_slist_append(headers, header); g_free(header);
    GString *buffer = g_string_new(NULL);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_object_to_json_string_ext(body, JSON_C_TO_STRING_PLAIN));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L); curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L); curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive); curl_easy_setopt(curl, CURLOPT_WRITEDATA, buffer);
    CURLcode result = curl_easy_perform(curl); long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    json_object *reply = sl_json_parse(buffer->str, buffer->len), *receipt = NULL;
    if (reply) json_object_object_get_ex(reply, "data", &receipt);
    int ok = result == CURLE_OK && status == 200 && yes(reply, "success") && yes(receipt, "verified") &&
        !strcmp(sl_string(receipt, "hubId"), hub) && !strcmp(sl_string(receipt, "channelBinding"), binding);
    sl_debug("media read authorization transport=%d status=%ld verified=%d", (int)result, status, ok);
    json_object_put(reply); json_object_put(body); g_string_free(buffer, TRUE);
    curl_slist_free_all(headers); curl_easy_cleanup(curl); g_free(url); return ok;
}
