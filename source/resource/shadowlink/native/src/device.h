#ifndef SL_DEVICE_H
#define SL_DEVICE_H
#include "runtime.h"
void sl_privacy_states(const char *path, int states[3]);
int sl_volume(int set_percent);
int sl_backlight(int set_percent);
int sl_backlight_at(const char *class_directory, int set_percent);
char *sl_wifi_ssid(void);
int sl_wifi_ip(uint8_t ip[4]);
/* Returns 1 on success, 0 on other failures, or SL_WIFI_NOT_FOUND. */
#define SL_WIFI_NOT_FOUND (-1)
int sl_wifi_connect(const char *ssid, const char *password, uint8_t ip[4]);
void sl_storage(uint64_t usage[4]);
int sl_binding_lock(void);
int sl_binding_shared_lock(void);
int sl_authorize_unbind(sl_runtime *runtime, const char *action);
int sl_prepare_recover(sl_runtime *runtime);
gboolean sl_unbind_pending(sl_runtime *runtime);
gboolean sl_unbind_blocks_binding(sl_runtime *runtime);
int sl_resume_unbind(sl_runtime *runtime);
int sl_recover_unbind(sl_runtime *runtime);
json_object *sl_unbind_journal(sl_runtime *runtime);
int sl_unbind_identity_matches(sl_runtime *runtime, json_object *journal);
int sl_retire_completed_unbind(sl_runtime *runtime);
/* preserve_session selects Status Recover: local cleanup without cloud unbind. */
int sl_unbind(sl_runtime *runtime, gboolean preserve_session);
int sl_reset_services(sl_runtime *runtime, gboolean from_shadowlink, gboolean *accepted);
int sl_reset_lock_held(int fd);
gboolean sl_reset_in_progress(void);
#endif
