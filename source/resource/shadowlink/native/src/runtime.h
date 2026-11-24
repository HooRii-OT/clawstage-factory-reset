#ifndef SL_RUNTIME_H
#define SL_RUNTIME_H
#include "shadowlink.h"
#include <gio/gio.h>
#include <json-c/json.h>
#include <limits.h>

typedef struct {
    char release[PATH_MAX], config[PATH_MAX], ready[PATH_MAX], adapter[128];
    uint8_t private_key[32], public_key[65], responder_id[16];
    uint32_t passcode;
    uint16_t discriminator;
    sl_session session;
    sl_receiver receiver;
    int last_status, recovered, after_action;
    gboolean operation_busy, binding_pending, invalidate_peers;
    char *pending_userinfo;
    char pending_activation[128];
    gboolean ota_authenticated, ota_active;
    gboolean advertisement_changed, reset_after_response;
    int binding_lock_fd;
    gboolean userinfo_busy, commissioning_response_sent;
    void *userinfo_context;
    int (*defer_userinfo)(void *context, const char *value, const char *hub);
    void (*cancel_userinfo)(void *context);
    int (*share_userinfo_lock)(void *context);
} sl_runtime;

char *sl_media_binding(sl_runtime *runtime, const char *hub);
int sl_media_consume(json_object *userinfo, const char *id, const char *binding);
json_object *sl_media_read(sl_runtime *runtime, json_object *request, const char *root);

void sl_debug_enable(gboolean enabled);
void sl_debug(const char *format, ...) G_GNUC_PRINTF(1, 2);
void sl_log(const char *format, ...) G_GNUC_PRINTF(1, 2);
char *sl_log_redact(const char *message);

const char *sl_string(json_object *object, const char *key);
json_object *sl_section(json_object *object, const char *key);
json_object *sl_commissioning(json_object *config);
json_object *sl_json_parse(const char *data, size_t length);
json_object *sl_json_read(const char *path);
int sl_json_write(const char *path, json_object *object);
int sl_unhex(const char *text, uint8_t *out, size_t length);
char *sl_hex(const uint8_t *bytes, size_t length);
int sl_identity_load(sl_runtime *runtime);
int sl_identity_init(sl_runtime *runtime, const char *version, gboolean allow_create);
int sl_commissioning_status(json_object *config);
/* Binding commit is independent of the optional Home Assistant token. */
gboolean sl_binding_committed(json_object *config);
int sl_config_reset(sl_runtime *runtime);
int sl_run(const char *const argv[], unsigned timeout_seconds, char **output);
int sl_run_with_stderr(const char *const argv[], unsigned timeout_seconds, char **output);
/* Only for the reset lifecycle's support-safe output, never arbitrary commands. */
int sl_run_logged(const char *const argv[], unsigned timeout_seconds, const char *marker, gboolean *seen);
int sl_http(const char *url, const char *body, const char *authorization,
            unsigned timeout_seconds, char **response, long *status);
int sl_websocket_send(const char *url, const char *json, double timeout);
int sl_websocket_event(const char *json);
int sl_notify_unbind_reset(void);
json_object *sl_update_call(const char *operation, json_object *arguments, char **code, char **error);
int sl_handle_frame(sl_runtime *runtime, const uint8_t *frame, size_t length,
                    uint8_t out[SL_MAX_FRAME], size_t *out_length);
void sl_after_response(sl_runtime *runtime, gboolean sent);
int sl_userinfo_response(sl_runtime *runtime, gboolean success, const char *value, const char *hub,
                         uint8_t out[SL_MAX_FRAME], size_t *length);
void sl_flush_userinfo(sl_runtime *runtime);
int sl_update_report(json_object *event, const char *hub_id, const char *legacy_session,
                     uint8_t out[SL_MAX_FRAME], size_t *length);
int sl_ble_run(sl_runtime *runtime, gboolean session_bus);
#endif
