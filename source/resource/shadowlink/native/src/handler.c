#define _GNU_SOURCE
#include "runtime.h"
#include "constants.h"
#include "device.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct { uint8_t data[SL_MAX_FRAME]; size_t length; int ok; } payload;
static void put(payload *p, unsigned tag, const void *data, size_t n) { if (p->ok) p->ok = sl_tlv_put(p->data, sizeof(p->data), &p->length, (uint16_t)tag, data, n); }
static void byte(payload *p, unsigned tag, unsigned value) { uint8_t b = (uint8_t)value; put(p, tag, &b, 1); }
static void string(payload *p, unsigned tag, const char *text) { put(p, tag, text, strlen(text)); }
static void u32(payload *p, unsigned tag, uint32_t value) { uint8_t b[4]; for (int i = 0; i < 4; ++i) b[i] = (uint8_t)(value >> (24 - 8 * i)); put(p, tag, b, 4); }
static void u64(payload *p, unsigned tag, uint64_t value) { uint8_t b[8]; for (int i = 0; i < 8; ++i) b[i] = (uint8_t)(value >> (56 - 8 * i)); put(p, tag, b, 8); }
static int field(const uint8_t *data, size_t length, unsigned tag, sl_tlv *out)
{
    size_t offset = 0; sl_tlv entry; int result, found = 0;
    *out = (sl_tlv){0};
    while ((result = sl_tlv_next(data, length, &offset, &entry)) > 0) {
        if (entry.tag == tag) { if (found) return 0; *out = entry; found = 1; }
    }
    return result == 0 && found;
}
static char *text_field(const uint8_t *data, size_t length, unsigned tag, size_t max)
{
    sl_tlv entry;
    if (!field(data, length, tag, &entry) || !entry.length || entry.length > max ||
        memchr(entry.value, 0, entry.length) || !g_utf8_validate((const char *)entry.value, (gssize)entry.length, NULL)) return NULL;
    return g_strndup((const char *)entry.value, entry.length);
}
static int response(unsigned id, payload *p, const char *hub, sl_runtime *r, gboolean encrypt,
                     uint8_t *out, size_t *length)
{
    size_t n = 4;
    sl_tlv status;
    sl_debug("response command=0x%02x encrypted=%d payload_bytes=%zu status=%d", id, encrypt, p->length,
        field(p->data, p->length, TAG_STATUS, &status) && status.length == 1 ? status.value[0] : -1);
    if (!p->ok) return 0;
    if (hub && *hub && !sl_tlv_put(out, SL_MAX_FRAME, &n, TAG_HUB_ID, (const uint8_t *)hub, strlen(hub))) return 0;
    size_t body = p->length;
    if (encrypt) { if (!sl_session_encrypt(&r->session, p->data, p->length, out + n, SL_MAX_FRAME - n, &body)) { sl_debug("response encryption failed session=%d tx_counter=%u", r->session.state, r->session.outgoing_counter); return 0; } }
    else { if (body > SL_MAX_FRAME - n) return 0; memcpy(out + n, p->data, body); }
    n += body;
    out[0] = 5; out[1] = (uint8_t)id; out[2] = (uint8_t)((n - 4) >> 8); out[3] = (uint8_t)(n - 4);
    *length = n; return 1;
}
static void device_status(sl_runtime *r, json_object *config, payload *p)
{
    int status = sl_unbind_blocks_binding(r) ? 2 : sl_commissioning_status(config); r->last_status = status;
    if (status == 2) r->recovered = 0;
    byte(p, TAG_COMMISSIONING_STATUS, (unsigned)status);
    char *ssid = sl_wifi_ssid(); string(p, TAG_SSID, ssid);
    gboolean wifi_connected = *ssid != 0; g_free(ssid);
    uint8_t ip[4] = {0}; put(p, TAG_IP_ADDRESS, ip, sl_wifi_ip(ip) ? 4 : 0);
    uint64_t usage[4]; sl_storage(usage);
    for (int disk = 0; disk < 2; ++disk) {
        uint8_t encoded[16];
        for (int field_index = 0; field_index < 2; ++field_index)
            for (int i = 0; i < 8; ++i) encoded[field_index * 8 + i] = (uint8_t)(usage[disk * 2 + field_index] >> (56 - 8 * i));
        put(p, disk ? TAG_SSD_STORAGE : TAG_SD_STORAGE, encoded, sizeof(encoded));
    }
    int volume = sl_volume(-1), backlight = sl_backlight(-1);
    byte(p, TAG_SYSTEM_VOLUME, (unsigned)volume); byte(p, TAG_BACKLIGHT_BRIGHTNESS, (unsigned)backlight);
    json_object *env = NULL, *ui = NULL;
    json_object_object_get_ex(config, "environment", &env); json_object_object_get_ex(config, "userinfo", &ui);
    const char *version = sl_string(env, "stage_version"); if (!*version) version = sl_string(config, "stage_version");
    char *clean_version = g_strdup(version); g_strstrip(clean_version);
    string(p, TAG_STAGE_VERSION, strlen(clean_version) <= 64 ? clean_version : ""); g_free(clean_version);
    char *token = g_strdup(sl_string(ui, "ha_token")); g_strstrip(token); long http_status;
    byte(p, TAG_HA_CONFIGURED, *token != 0);
    gboolean online = *token && sl_http("http://127.0.0.1:8123/api/", NULL, token, 1, NULL, &http_status) && http_status >= 200 && http_status < 300;
    gboolean ha_online = online, ha_configured = *token != 0;
    byte(p, TAG_HA_ONLINE, online); g_free(token);
    online = sl_http("http://127.0.0.1:18800/mate-status", NULL, NULL, 1, NULL, &http_status) && http_status >= 200 && http_status < 300;
    byte(p, TAG_OPENCLAW_ONLINE, online);
    int states[3] = {-1, -1, -1};
    if (sl_run((const char *const[]){"systemctl", "is-active", "--quiet", "privacy-button-state.service", NULL}, 2, NULL))
        sl_privacy_states("/run/clawstage/privacy-button-state.json", states);
    byte(p, TAG_PRIVACY_BUTTON_STATE, (unsigned)states[0]);
    byte(p, TAG_CAMERA_STATE, (unsigned)states[1]);
    byte(p, TAG_MICROPHONE_STATE, (unsigned)states[2]);
    sl_debug("device status reply=0x0b commissioning=%d wifi_connected=%d volume=%d backlight=%d ha_configured=%d ha_online=%d openclaw_online=%d privacy=%d camera=%d mic=%d sd_used=%llu sd_total=%llu ssd_used=%llu ssd_total=%llu",
        status, wifi_connected, volume, backlight, ha_configured, ha_online, online, states[0], states[1], states[2],
        (unsigned long long)usage[0], (unsigned long long)usage[1], (unsigned long long)usage[2], (unsigned long long)usage[3]);
}
static char *byok_detail(json_object *reply, const char *raw)
{
    const char *detail = "";
    const char *keys[] = {"error", "message"};
    for (size_t i = 0; i < G_N_ELEMENTS(keys); ++i) {
        const char *candidate = sl_string(reply, keys[i]);
        if (*candidate) { detail = candidate; break; }
    }
    if (!*detail) detail = raw ? raw : "";
    char *valid = g_utf8_make_valid(detail, -1); g_strstrip(valid);
    char *short_detail = g_utf8_substring(valid, 0, MIN(512, g_utf8_strlen(valid, -1)));
    g_free(valid); return short_detail;
}
static char *user_info_text(const uint8_t *data, size_t n)
{
    char *value = text_field(data, n, TAG_USER_INFO, 8192);
    if (!value && n >= 2 && data[0] == TAG_USER_INFO && data[1] >= 0x80 && (size_t)data[1] == n - 2 &&
        !memchr(data + 2, 0, n - 2) && g_utf8_validate((const char *)data + 2, (gssize)n - 2, NULL)) value = g_strndup((const char *)data + 2, n - 2);
    return value;
}
static gboolean same_user_info(json_object *config, const uint8_t *data, size_t n)
{
    char *text = user_info_text(data, n);
    json_object *parsed = text ? sl_json_parse(text, strlen(text)) : NULL, *ui = NULL;
    gboolean same = json_object_is_type(parsed, json_type_object) &&
        json_object_object_get_ex(config, "userinfo", &ui);
    const char *keys[] = {"hub_token", "union_id", "hub_id"};
    for (size_t i = 0; same && i < G_N_ELEMENTS(keys); ++i) {
        json_object *entry = NULL;
        if (json_object_object_get_ex(parsed, keys[i], &entry))
            same = json_object_is_type(entry, json_type_string) &&
                !strcmp(json_object_get_string(entry), sl_string(ui, keys[i]));
    }
    json_object_put(parsed); g_free(text); return same;
}
static int user_config(sl_runtime *r, json_object *config, const uint8_t *data, size_t n, gboolean token, const char *hub)
{
    gboolean unchanged = !token && same_user_info(config, data, n);
    char *value = token ? text_field(data, n, TAG_HA_TOKEN, 8192) : user_info_text(data, n);
    if (!value && token) {
        sl_tlv entry;
        if (field(data, n, TAG_HA_TOKEN, &entry) && !entry.length) value = g_strdup("");
    }
    if (!value) return 0;
    json_object *ui = sl_section(config, "userinfo"), *parsed = sl_json_parse(value, strlen(value));
    int ok = 0;
    if (token) {
        json_object *wrapped = NULL;
        if (json_object_is_type(parsed, json_type_object) && json_object_object_get_ex(parsed, "long_live_token", &wrapped)) {
            if (!json_object_is_type(wrapped, json_type_string)) goto done;
            json_object_object_add(ui, "ha_token", json_object_get(wrapped));
        } else json_object_object_add(ui, "ha_token", json_object_new_string(value));
    } else {
        if (!json_object_is_type(parsed, json_type_object)) goto done;
        const char *keys[] = {"hub_token", "union_id", "hub_id"};
        for (size_t i = 0; i < G_N_ELEMENTS(keys); ++i) {
            json_object *entry = NULL;
            if (json_object_object_get_ex(parsed, keys[i], &entry)) {
                if (!json_object_is_type(entry, json_type_string)) goto done;
                json_object_object_add(ui, keys[i], json_object_get(entry));
            }
        }
    }
    if (!token && r->cancel_userinfo) r->cancel_userinfo(r->userinfo_context);
    ok = unchanged || sl_json_write(r->config, config);
    if (ok && !token) {
        if (r->defer_userinfo) {
            ok = r->defer_userinfo(r->userinfo_context, value, hub) ? 2 : 0;
            goto done;
        }
        char *script = g_build_filename(r->release, "openclaw", "hoorii_claw", "script", "internal", "provision-runtime.py", NULL);
        ok = sl_run((const char *const[]){"/usr/bin/python3", script, "enqueue", r->release, NULL}, 30, NULL); g_free(script);
        if (ok) { g_free(r->pending_userinfo); r->pending_userinfo = g_strdup(value); }
    }
 done:
    json_object_put(parsed); g_free(value); return ok;
}
static void ota_request(sl_runtime *r, unsigned id, const uint8_t *data, size_t n, payload *p)
{
    char *version = text_field(data, n, TAG_TARGET_VERSION, 64);
    char *target = text_field(data, n, id == 0x12 ? TAG_TARGET_URL : TAG_UPDATE_SESSION_ID, id == 0x12 ? 4096 : 64);
    char *code = NULL, *error = NULL;
    json_object *arguments = json_object_new_object(), *result = NULL;
    if (!version || !target) { code = g_strdup("INVALID_REQUEST"); error = g_strdup("Missing or invalid update fields"); goto done; }
    json_object_object_add(arguments, "target_version", json_object_new_string(version));
    if (id == 0x12) {
        json_object_object_add(arguments, "target_url", json_object_new_string(target));
        result = sl_update_call("prepare_update", arguments, &code, &error);
        const char *transaction = sl_string(result, "transaction_id");
        if (!code && (!*transaction || strlen(transaction) >= sizeof(r->pending_activation))) { code = g_strdup("INTERNAL_ERROR"); error = g_strdup("Invalid OTA transaction ID"); }
        if (!code) { g_strlcpy(r->pending_activation, transaction, sizeof(r->pending_activation)); r->after_action = 3; }
    } else {
        sl_tlv action, sequence; uint32_t after = 0;
        if (!field(data, n, TAG_UPDATE_SESSION_ACTION, &action) || action.length != 1 || action.value[0] > 1) {
            code = g_strdup("INVALID_REQUEST"); error = g_strdup("Invalid session action"); goto done;
        }
        if (field(data, n, TAG_LAST_RECEIVED_SEQUENCE, &sequence)) {
            if (sequence.length != 4) { code = g_strdup("INVALID_REQUEST"); error = g_strdup("Invalid sequence"); goto done; }
            for (int i = 0; i < 4; ++i) after = (after << 8) | sequence.value[i];
        }
        json_object_object_add(arguments, "session_id", json_object_new_string(target));
        json_object_object_add(arguments, "action", json_object_new_string(action.value[0] ? "RESUME" : "START"));
        json_object_object_add(arguments, "last_received_sequence", json_object_new_int64(after));
        result = sl_update_call("bind_session", arguments, &code, &error);
    }
 done:
    if (id == 0x14) string(p, TAG_UPDATE_SESSION_ID, target ? target : "");
    string(p, TAG_TARGET_VERSION, version ? version : ""); byte(p, TAG_STATUS, code ? 1 : 0);
    if (id == 0x12 && !code) { byte(p, TAG_OTA_PROTOCOL_VERSION, 3); string(p, TAG_OTA_TRANSACTION_ID, r->pending_activation); }
    if (id == 0x14) { json_object *sequence = NULL; if (result) json_object_object_get_ex(result, "sequence", &sequence); u32(p, TAG_UPDATE_SEQUENCE, (uint32_t)json_object_get_int64(sequence)); }
    if (code) string(p, TAG_UPDATE_ERROR_CODE, code);
    if (error) { char *short_error = g_utf8_substring(error, 0, MIN(240, g_utf8_strlen(error, -1))); string(p, TAG_ERROR_MESSAGE, short_error); g_free(short_error); }
    json_object_put(arguments); json_object_put(result); g_free(version); g_free(target); g_free(code); g_free(error);
}
static void operation_busy(payload *p, sl_runtime *r, unsigned command, const char *reason)
{
    sl_debug("request rejected command=0x%02x code=OPERATION_BUSY reason=%s binding_pending=%d userinfo_busy=%d commissioning_response_sent=%d",
        command, reason, r->binding_pending, r->userinfo_busy, r->commissioning_response_sent);
    byte(p, TAG_STATUS, 1);
    string(p, TAG_OPERATION_ERROR_CODE, "OPERATION_BUSY");
    string(p, TAG_ERROR_MESSAGE, "Another device operation is in progress");
}
static int binding_request_lock(sl_runtime *r, gboolean identity_write)
{
    if (r->userinfo_busy)
        return r->session.state == 2 && r->share_userinfo_lock ?
            r->share_userinfo_lock(r->userinfo_context) : -1;
    return identity_write ? sl_binding_lock() : sl_binding_shared_lock();
}
int sl_handle_frame(sl_runtime *r, const uint8_t *frame, size_t length, uint8_t out[SL_MAX_FRAME], size_t *out_length)
{
    *out_length = 0; r->after_action = 0;
    if (length < 4 || length != 4 + (((size_t)frame[2] << 8) | frame[3])) {
        sl_debug("frame rejected: invalid length bytes=%zu", length); return 0;
    }
    const uint8_t *data = frame + 4; size_t n = length - 4; unsigned id = frame[1];
    sl_debug("request protocol=0x%02x command=0x%02x bytes=%zu session=%d rx_counter=%u tx_counter=%u",
        frame[0], id, n, r->session.state, r->session.incoming_counter, r->session.outgoing_counter);
    if (frame[0] == 0 && id == 0 && n) {
        r->ota_authenticated = FALSE;
        if (*data == 0x9e) {
            int ok = sl_auth1(&r->session, data, n, r->private_key, r->responder_id, r->passcode, out, SL_MAX_FRAME, out_length);
            sl_debug("AUTH1 result=%d session=%d response_bytes=%zu", ok, r->session.state, *out_length);
            if (ok) r->after_action = 1;
            return ok;
        }
        if (r->cancel_userinfo) r->cancel_userinfo(r->userinfo_context);
        r->commissioning_response_sent = FALSE; r->binding_pending = FALSE;
        r->last_status = -1; r->recovered = 0; g_clear_pointer(&r->pending_userinfo, g_free);
        int ok = sl_auth0(&r->session, data, n, r->responder_id, out, SL_MAX_FRAME, out_length);
        sl_debug("AUTH0 result=%d session=%d response_bytes=%zu", ok, r->session.state, *out_length);
        return ok;
    }
    if (frame[0] != 5) { sl_debug("frame rejected: unsupported protocol"); return 0; }
    json_object *config = sl_json_read(r->config), *ui = NULL;
    if (!config) { sl_debug("request rejected: device config unreadable or invalid"); return 0; }
    json_object_object_get_ex(config, "userinfo", &ui);
    char *hub = NULL;
    int ok = 0;
    if (n && data[0] == TAG_HUB_ID) {
        size_t offset = 0; sl_tlv envelope;
        if (sl_tlv_next(data, n, &offset, &envelope) == 1 && envelope.length &&
            !memchr(envelope.value, 0, envelope.length) && g_utf8_validate((const char *)envelope.value, (gssize)envelope.length, NULL)) {
            char *candidate = g_strndup((const char *)envelope.value, envelope.length);
            char *configured = g_strdup(sl_string(ui, "hub_id")); g_strstrip(configured);
            if (*configured && !strcmp(candidate, configured)) { hub = candidate; data += offset; n -= offset; }
            else g_free(candidate);
            g_free(configured);
        }
        if (!hub && !(r->session.state == 2 && n >= 16)) {
            sl_debug("request rejected: invalid or mismatched hub envelope"); goto done;
        }
        /* Ciphertext can also begin with TAG_HUB_ID. Strip only a complete,
         * matching envelope; otherwise authenticated commands decrypt it whole. */
    }
    sl_debug("command=0x%02x matching_hub_envelope=%d", id, hub != NULL);
    if ((id == 0x12 || id == 0x14 || id == 0x15) && !hub) {
        sl_debug("request rejected: required hub envelope missing or mismatched"); goto done;
    }
    payload p = {.ok = 1}; uint8_t plain[SL_MAX_FRAME]; size_t plain_length;
    if (id == 6) g_clear_pointer(&r->pending_userinfo, g_free);
    gboolean encrypted_response = id == 0 || id == 0x0e;
    gboolean decrypt = id == 0 || id == 6 || id == 8 || id == 12 || id == 14 ||
        (id == 2 && r->session.state == 2) || ((id == 10 || id == 16 || id == 23) && r->session.state == 2 && n >= 16);
    if (id == 10 || id == 16 || id == 23) encrypted_response = decrypt;
    uint32_t request_counter = r->session.incoming_counter;
    if (decrypt) {
        if (!sl_session_decrypt(&r->session, data, n, plain, sizeof(plain), &plain_length)) {
            const char *reason = r->session.state != 2 ? "session_not_authenticated" :
                n < 16 ? "ciphertext_too_short" : r->session.incoming_counter > 65535 ? "counter_exhausted" :
                "authentication_tag_mismatch_or_crypto_error";
            sl_debug("decrypt rejected: command=0x%02x reason=%s session=%d expected_counter=%u; reconnect and authenticate",
                id, reason, r->session.state, r->session.incoming_counter);
            /* Python returns plaintext failure status for these configuration
             * handlers, including authentication/decryption exceptions. */
            if (id == 2 || id == 6 || id == 8 || id == 12) {
                if (id != 2 && r->session.state == 2 && n >= 16 && request_counter <= 65535)
                    ++r->session.incoming_counter;
                byte(&p, TAG_STATUS, 1);
                if (id == 12) string(&p, TAG_ERROR_MESSAGE, r->session.state != 2 ?
                    (r->session.state == 1 ? "No encryption key" : "No encryption context") : n < 16 ? "Payload too short" : "Decryption failed");
                ok = response(id + 1, &p, hub, r, FALSE, out, out_length);
            }
            goto done;
        }
        sl_debug("decrypt ok command=0x%02x plaintext_bytes=%zu counter=%u", id, plain_length, request_counter);
        data = plain; n = plain_length;
        if ((id == 2 || id == 14) && n) {
            r->session.incoming_counter = request_counter;
            if (id == 14) { sl_debug("request rejected: recover payload must be empty"); goto done; }
        }
    }
    unsigned reply_id = id == 0x17 ? 0x18 : id + 1;
    /* The durable request, including the not-yet-confirmed case, survives the
     * maintenance flock. Do not let provisioning replace its credentials. */
    if ((id == 2 || id == 6 || id == 8 || id == 12 || id == 18 || id == 20) && sl_unbind_blocks_binding(r)) {
        json_object *journal = sl_unbind_journal(r), *confirmed = NULL;
        if (journal) json_object_object_get_ex(journal, "confirmed", &confirmed);
        operation_busy(&p, r, id, !journal ? "unbind_journal_invalid" :
            json_object_get_boolean(confirmed) ? "unbind_cleanup_pending" : "unbind_confirmation_pending");
        json_object_put(journal);
        ok = response(reply_id, &p, hub, r, encrypted_response, out, out_length);
        goto done;
    }
    if (r->operation_busy && id != 4) {
        operation_busy(&p, r, id, "operation_gate");
        if (id == 18 || id == 20) string(&p, TAG_UPDATE_ERROR_CODE, "OPERATION_BUSY");
        ok = response(reply_id, &p, hub, r, encrypted_response, out, out_length);
        goto done;
    }
    if ((id == 2 || id == 6 || id == 8) && sl_reset_in_progress()) {
        byte(&p, TAG_STATUS, 1);
        ok = response(reply_id, &p, hub, r, encrypted_response, out, out_length);
        goto done;
    }
    switch (id) {
    case 0: {
        char *ssid = text_field(data, n, TAG_SSID, 32), *password = text_field(data, n, TAG_PASSWORD, 128); uint8_t ip[4];
        sl_tlv password_entry;
        gboolean password_valid = !field(data, n, TAG_PASSWORD, &password_entry) || !password_entry.length || password != NULL;
        int result = ssid && password_valid ? sl_wifi_connect(ssid, password, ip) : 0;
        int success = result == 1;
        byte(&p, TAG_STATUS, success ? 0 : 1);
        if (success) { put(&p, TAG_IP_ADDRESS, ip, 4); char hostname[256] = {0}; if (!gethostname(hostname, sizeof(hostname)-1)) string(&p, TAG_HOSTNAME, hostname); }
        else string(&p, TAG_ERROR_MESSAGE, result == SL_WIFI_NOT_FOUND ?
            "WiFi network not found. If you are using 5 GHz WiFi, please switch to 2.4 GHz and try again." :
            ssid ? "WiFi connection or Home Assistant restart failed" : "Invalid WiFi configuration (missing SSID)");
        g_free(ssid); g_free(password); break;
    }
    case 2: {
        int lock = binding_request_lock(r, FALSE);
        gboolean clearing = sl_unbind_blocks_binding(r);
        int success = lock >= 0 && !clearing && !sl_reset_in_progress() && r->session.state == 2 && n == 0;
        if (success) {
            char *persistent = sl_hex(r->session.pending_persistent, 32);
            json_object *c = sl_commissioning(config);
            json_object_object_add(c, "kpersistent", json_object_new_string(persistent)); g_free(persistent);
            json_object_object_add(c, "commissioning_flag", json_object_new_int(1)); success = sl_json_write(r->config, config);
            if (success && !sl_retire_completed_unbind(r))
                sl_log("Shadowlink: old reset notification retirement deferred after commissioning\n");
        }
        if (lock >= 0) close(lock);
        byte(&p, TAG_STATUS, success ? 0 : 1);
        if (success) { r->binding_pending = FALSE; r->advertisement_changed = TRUE; r->after_action = 2; }
        sl_debug("commissioning commit success=%d binding_pending=%d userinfo_busy=%d",
            success, r->binding_pending, r->userinfo_busy);
        break;
    }
    case 4: {
        /* The mobile flow resets only after AUTH1. Never let a newly connected
         * peer erase user data with an unauthenticated four-byte request. */
        if (r->session.state != 2 || !r->ota_authenticated) {
            sl_debug("factory reset rejected: current link has not completed AUTH1");
            byte(&p, TAG_STATUS, 1); break;
        }
        /* Apps may encrypt the empty reset payload (a 16-byte authentication
         * tag). Validate it and consume its nonce even if maintenance is busy.
         * The reset response remains plaintext, as in the Python protocol. */
        if (n && !sl_session_decrypt(&r->session, data, n, plain, sizeof(plain), &plain_length)) {
            sl_debug("factory reset rejected: invalid encrypted payload bytes=%zu expected_counter=%u", n, r->session.incoming_counter);
            byte(&p, TAG_STATUS, 1); break;
        }
        if (n && plain_length) {
            sl_debug("factory reset rejected: decrypted payload must be empty");
            byte(&p, TAG_STATUS, 1); break;
        }
        if (r->operation_busy) { operation_busy(&p, r, id, "operation_gate"); break; }
        if (r->userinfo_busy) { byte(&p, TAG_STATUS, 1); break; }
        sl_debug("factory reset accepted payload: encrypted=%d rx_counter=%u", n != 0, r->session.incoming_counter);
        int success = sl_unbind(r, FALSE);
        byte(&p, TAG_STATUS, success ? 0 : 1);
        break;
    }
    case 6: case 8: {
        int lock = binding_request_lock(r, id == 6);
        if (lock < 0 && id == 6 && !r->userinfo_busy) {
            // Identical retransmission must reach the durable queue even while
            // its worker holds a shared lease. Never allow identity replacement.
            lock = sl_binding_shared_lock();
            if (lock >= 0) {
                json_object *latest = sl_json_read(r->config);
                if (!latest || !same_user_info(latest, data, n)) {
                    close(lock); lock = -1;
                } else {
                    json_object_put(config); config = json_object_get(latest);
                }
                json_object_put(latest);
            }
        }
        gboolean clearing = sl_unbind_blocks_binding(r);
        /* Refreshing credentials for an already commissioned owner does not
         * start another commissioning transaction. Keep a real pending bind
         * (including an identity replacement) exclusive until its own commit. */
        gboolean already_commissioned = !r->binding_pending && sl_binding_committed(config) &&
            (id == 8 || same_user_info(config, data, n));
        r->binding_lock_fd = lock;
        int result = lock >= 0 && !clearing && !sl_reset_in_progress() ? user_config(r, config, data, n, id == 8, hub) : 0;
        if (lock >= 0) close(lock);
        if (result) r->binding_pending = !already_commissioned;
        sl_debug("binding write command=0x%02x result=%d already_commissioned=%d binding_pending=%d userinfo_busy=%d",
            id, result, already_commissioned, r->binding_pending, r->userinfo_busy);
        if (result == 2) { ok = 1; goto done; }
        byte(&p, TAG_STATUS, result ? 0 : 1); break;
    }
    case 10:
        device_status(r, config, &p); break;
    case 12: {
        sl_tlv value;
        char *request = NULL, *reply = NULL, *error = NULL; long status = 0;
        if (field(data, n, TAG_BYOK_CONFIG, &value) && !memchr(value.value, 0, value.length))
            request = g_strndup((const char *)value.value, value.length);
        json_object *read_request = request ? sl_json_parse(request, strlen(request)) : NULL;
        if (!strcmp(sl_string(read_request, "operation"), "read-media")) {
            char *root = g_build_filename(r->release, "user", ".hoorii", NULL);
            json_object *media = root ? sl_media_read(r, read_request, root) : NULL;
            encrypted_response = TRUE;
            byte(&p, TAG_STATUS, media ? 0 : 1);
            if (media) string(&p, TAG_BYOK_CONFIG, json_object_to_json_string_ext(media, JSON_C_TO_STRING_PLAIN));
            else string(&p, TAG_ERROR_MESSAGE, "Media sync authorization failed; update app/device, check connectivity and retry");
            json_object_put(media); json_object_put(read_request); g_free(root); g_free(request); break;
        }
        json_object_put(read_request);
        gboolean transported = request && sl_http("http://127.0.0.1:18789/hoorii/byok/profile", request, NULL, 10, &reply, &status);
        json_object *result = reply ? sl_json_parse(reply, strlen(reply)) : NULL, *success = NULL;
        if (!request) error = g_strdup("BYOK config (Tag 0x1a) not found");
        else if (!transported) error = g_strdup("BYOK profile endpoint unavailable");
        else if (status < 200 || status >= 300) error = g_strdup_printf("BYOK profile endpoint returned HTTP %ld", status);
        else if (!result) error = g_strdup("BYOK profile endpoint returned invalid JSON");
        else if (!json_object_object_get_ex(result, "success", &success) ||
            !json_object_is_type(success, json_type_boolean) || !json_object_get_boolean(success))
            error = g_strdup("BYOK profile endpoint rejected the request");
        if (error && transported && ((status < 200 || status >= 300) || result)) {
            char *detail = byok_detail(result, reply);
            if (*detail) { char *with_detail = g_strconcat(error, ": ", detail, NULL); g_free(error); error = with_detail; }
            g_free(detail);
        }
        byte(&p, TAG_STATUS, error ? 1 : 0);
        if (error) string(&p, TAG_ERROR_MESSAGE, error);
        json_object_put(result); g_free(request); g_free(reply); g_free(error); break;
    }
    case 14: {
        if (r->userinfo_busy) { byte(&p, TAG_STATUS, 1); break; }
        gboolean notification_only = sl_unbind_pending(r) && !sl_unbind_blocks_binding(r);
        int success = n == 0 && !sl_reset_in_progress() &&
            (r->recovered || (sl_unbind_pending(r) ? sl_resume_unbind(r) : (r->last_status == 2 && sl_unbind(r, TRUE))));
        if (success && !notification_only) { r->recovered = 1; r->last_status = 1; g_clear_pointer(&r->pending_userinfo, g_free); r->advertisement_changed = TRUE; }
        byte(&p, TAG_STATUS, success ? 0 : 1); break;
    }
    case 16: case 23: {
        unsigned tag = id == 16 ? TAG_SYSTEM_VOLUME : TAG_BACKLIGHT_BRIGHTNESS; sl_tlv value;
        int success = field(data, n, tag, &value) && value.length == 1 && value.value[0] <= 100;
        sl_debug("%s request valid=%d percent=%d", id == 16 ? "volume" : "backlight", success,
            success ? value.value[0] : -1);
        int current = -1;
        if (success) current = id == 16 ? sl_volume(value.value[0]) : sl_backlight(value.value[0]);
        success = current >= 0;
        byte(&p, TAG_STATUS, success ? 0 : 1);
        int readback = -1;
        if (success) {
            readback = id == 16 ? sl_volume(-1) : sl_backlight(-1);
            if (readback < 0) readback = value.value[0];
            byte(&p, tag, (unsigned)readback);
        }
        sl_debug("%s result=%d readback_percent=%d status=%d", id == 16 ? "volume" : "backlight", current, readback, success ? 0 : 1);
        break;
    }
    case 18: case 20:
        ota_request(r, id, data, n, &p); break;
    case 21: {
        char *transaction = text_field(data, n, TAG_OTA_TRANSACTION_ID, 64); sl_tlv sequence, status;
        if (transaction && field(data, n, TAG_UPDATE_SEQUENCE, &sequence) && sequence.length == 4 &&
            field(data, n, TAG_STATUS, &status) && status.length == 1 && status.value[0] == 0) {
            uint32_t value = 0; for (int i = 0; i < 4; ++i) value = (value << 8) | sequence.value[i];
            json_object *args = json_object_new_object();
            json_object_object_add(args, "transaction_id", json_object_new_string(transaction));
            json_object_object_add(args, "sequence", json_object_new_int64(value));
            char *code = NULL; json_object *result = sl_update_call("acknowledge", args, &code, NULL);
            ok = code == NULL; g_free(code); json_object_put(result); json_object_put(args);
        }
        g_free(transaction); goto done;
    }
    default: sl_debug("request rejected: unsupported command=0x%02x", id); goto done;
    }
    ok = response(reply_id, &p, hub, r, encrypted_response, out, out_length);
 done:
    sl_debug("request finished command=0x%02x handled=%d response_bytes=%zu session=%d rx_counter=%u tx_counter=%u",
        id, ok, *out_length, r->session.state, r->session.incoming_counter, r->session.outgoing_counter);
    json_object_put(config); g_free(hub); return ok;
}
int sl_userinfo_response(sl_runtime *r, gboolean success, const char *value, const char *hub,
                         uint8_t out[SL_MAX_FRAME], size_t *length)
{
    if (success) { g_free(r->pending_userinfo); r->pending_userinfo = g_strdup(value); }
    payload p = {.ok = 1}; byte(&p, TAG_STATUS, success ? 0 : 1);
    return response(7, &p, hub, r, FALSE, out, length);
}
void sl_flush_userinfo(sl_runtime *r)
{
    if (r->commissioning_response_sent && r->pending_userinfo) {
        json_object *event = json_object_new_object(); json_object_object_add(event, "user_info", json_object_new_string(r->pending_userinfo));
        if (sl_websocket_event(json_object_to_json_string_ext(event, JSON_C_TO_STRING_PLAIN))) g_clear_pointer(&r->pending_userinfo, g_free);
        json_object_put(event);
    }
}
void sl_after_response(sl_runtime *r, gboolean sent)
{
    if (sent && r->after_action == 1) r->ota_authenticated = TRUE;
    if (sent && r->after_action == 2) { r->commissioning_response_sent = TRUE; sl_flush_userinfo(r); }
    if (*r->pending_activation && r->after_action == 3) {
        json_object *args = json_object_new_object();
        json_object_object_add(args, "transaction_id", json_object_new_string(r->pending_activation));
        char *code = NULL;
        json_object *result = sl_update_call(sent ? "activate_update" : "cancel_prepared_update", args, &code, NULL);
        if (!code || !sent) r->pending_activation[0] = 0;
        json_object_put(result); json_object_put(args); g_free(code);
    }
    r->after_action = 0;
}
int sl_update_report(json_object *event, const char *hub, const char *legacy_session, uint8_t out[SL_MAX_FRAME], size_t *length)
{
    static const char *statuses[] = {"IDLE", "CHECKING", "DOWNLOADING", "VERIFYING", "INSTALLING", "PREPARING_SHADOWLINK", "STOPPING_SERVICES", "ACTIVATING_NEW_RELEASE", "STARTING_SHADOWLINK", "SHADOWLINK_READY", "INSTALLING_SYSTEM_DEPENDENCIES", "INSTALLING_DRIVERS", "PREPARING_COMPONENTS", "PREPARING_XMOS", "PREPARING_HARDWARE_SERVICES", "PREPARING_OPENCLAW", "PREPARING_HOMEASSISTANT", "PREPARING_STAGE_UI", "STARTING_COMPONENTS", "HEALTH_CHECKING", "REBOOT_PENDING", "POST_BOOT_VERIFYING", "ROLLING_BACK", "ROLLED_BACK", "ROLLBACK_FAILED", "SUCCEEDED", "FAILED"};
    const char *status = sl_string(event, "status");
    if (!*status) {
        status = sl_string(event, "stage");
        const char *old[] = {"REQUEST_ACCEPTED", "RESOLVING_PACKAGE", "VERIFYING_PACKAGE", "PREPARING_PACKAGE", "COMMITTING", "COMPLETED"};
        const char *mapped[] = {"CHECKING", "CHECKING", "VERIFYING", "INSTALLING", "HEALTH_CHECKING", "SUCCEEDED"};
        for (size_t i = 0; i < G_N_ELEMENTS(old); ++i) if (!strcmp(status, old[i])) { status = mapped[i]; break; }
        if (!*status) status = sl_string(event, "state");
    }
    if (!strcmp(status, "REBOOTING_STAGE")) status = "REBOOT_PENDING";
    size_t index; for (index = 0; index < G_N_ELEMENTS(statuses); ++index) if (!strcmp(status, statuses[index])) break;
    if (index == G_N_ELEMENTS(statuses)) return 0;
    payload p = {.ok = 1}; json_object *sequence = NULL; json_object_object_get_ex(event, "sequence", &sequence);
    if (legacy_session) string(&p, TAG_UPDATE_SESSION_ID, legacy_session);
    else { byte(&p, TAG_OTA_PROTOCOL_VERSION, 3); string(&p, TAG_OTA_TRANSACTION_ID, sl_string(event, "transaction_id")); }
    string(&p, TAG_TARGET_VERSION, sl_string(event, "target_version"));
    u32(&p, TAG_UPDATE_SEQUENCE, (uint32_t)json_object_get_int64(sequence)); byte(&p, TAG_UPDATE_STATUS, (unsigned)index);
    json_object *download = NULL, *bytes = NULL, *total = NULL, *percent = NULL;
    if (!strcmp(status, "DOWNLOADING") &&
        json_object_object_get_ex(event, "download", &download) &&
        json_object_object_get_ex(download, "downloaded_bytes", &bytes) &&
        json_object_is_type(bytes, json_type_int) && json_object_get_int64(bytes) >= 0) {
        u64(&p, TAG_DOWNLOAD_BYTES, (uint64_t)json_object_get_int64(bytes));
        json_object_object_get_ex(download, "total_bytes", &total);
        json_object_object_get_ex(download, "percent", &percent);
        if (json_object_is_type(total, json_type_int) && json_object_get_int64(total) > 0 &&
            json_object_get_int64(bytes) <= json_object_get_int64(total) &&
            json_object_is_type(percent, json_type_int) && json_object_get_int(percent) >= 0 &&
            json_object_get_int64(percent) <= 100) {
            u64(&p, TAG_DOWNLOAD_TOTAL_BYTES, (uint64_t)json_object_get_int64(total));
            byte(&p, TAG_DOWNLOAD_PERCENT, (unsigned)json_object_get_int(percent));
        }
    }
    const char *keys[] = {"error_code", "error_message", "timestamp"}; unsigned tags[] = {TAG_UPDATE_ERROR_CODE, TAG_ERROR_MESSAGE, TAG_UPDATE_TIMESTAMP};
    for (size_t i = 0; i < G_N_ELEMENTS(keys); ++i) {
        const char *value = sl_string(event, keys[i]);
        if (*value) { char *text = i == 1 ? g_utf8_substring(value, 0, MIN(240, g_utf8_strlen(value, -1))) : g_strdup(value); string(&p, tags[i], text); g_free(text); }
    }
    return response(legacy_session ? 0x16 : 0x14, &p, hub, NULL, FALSE, out, length);
}
