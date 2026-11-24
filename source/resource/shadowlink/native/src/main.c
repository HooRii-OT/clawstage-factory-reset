#define _GNU_SOURCE
#include "runtime.h"
#include "device.h"
#include <curl/curl.h>
#include <openssl/crypto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    sl_runtime runtime = {.last_status = -1};
    gboolean debug = g_strcmp0(g_getenv("SHADOWLINK_DEBUG"), "0") != 0;
    gboolean resume_unbind = FALSE;
    gboolean authorize_reset = FALSE, notify_reset = FALSE, reset = FALSE, reset_config_only = FALSE, initialize = FALSE, allow_create = FALSE, check = FALSE, stdio_mode = FALSE, session_bus = FALSE;
    int maintenance_lock_fd = -1;
    const char *version = "", *test_serial = NULL;
    g_strlcpy(runtime.ready, "/run/shadowlink/ready", sizeof(runtime.ready));
    g_strlcpy(runtime.adapter, "/org/bluez/hci0", sizeof(runtime.adapter));
    for (int i = 1; i < argc; ++i) {
        const char *option = argv[i];
        if (!strcmp(option, "--version")) { puts("shadowlink-native 0.1.0"); return 0; }
        if (!strcmp(option, "--help")) {
            puts("Usage: shadowlink-server --release-dir DIR [--config FILE] [--ready-file FILE] [--adapter /org/bluez/hci0]\n"
                 "  --initialize --stage-version VERSION [--allow-create]  Initialize device identity\n"
                 "  --factory-reset  Clear user configuration, reset services and restart BLE\n"
                 "  --resume-unbind  Resume an existing BLE reset journal (root, active release only)\n"
                 "  --check-config  Validate established identity without modifying it\n"
                 "  --debug  Enable diagnostic logs (default on; SHADOWLINK_DEBUG=0 disables); no credentials or raw payloads\n"
                 "  --stdio  Process hex frames from stdin (development tests)\n"
                 "  --session-bus --test-serial HEX16  Use an isolated BlueZ test bus"); return 0;
        }
        if (!strcmp(option, "--debug")) { debug = TRUE; continue; }
        if (!strcmp(option, "--resume-unbind")) { resume_unbind = TRUE; continue; }
        if (!strcmp(option, "--authorize-reset")) { authorize_reset = TRUE; continue; }
        if (!strcmp(option, "--factory-reset")) { reset = TRUE; continue; }
        if (!strcmp(option, "--notify-factory-reset")) { notify_reset = TRUE; continue; }
        if (!strcmp(option, "--reset-config-only")) { reset_config_only = TRUE; continue; }
        if (!strcmp(option, "--initialize")) { initialize = TRUE; continue; }
        if (!strcmp(option, "--allow-create")) { allow_create = TRUE; continue; }
        if (!strcmp(option, "--check-config")) { check = TRUE; continue; }
        if (!strcmp(option, "--stdio")) { stdio_mode = TRUE; continue; }
        if (!strcmp(option, "--session-bus")) { session_bus = TRUE; continue; }
        if (++i >= argc) { sl_log("Missing option value\n"); return 2; }
        if (!strcmp(option, "--release-dir")) g_strlcpy(runtime.release, argv[i], sizeof(runtime.release));
        else if (!strcmp(option, "--config")) g_strlcpy(runtime.config, argv[i], sizeof(runtime.config));
        else if (!strcmp(option, "--ready-file")) g_strlcpy(runtime.ready, argv[i], sizeof(runtime.ready));
        else if (!strcmp(option, "--adapter")) g_strlcpy(runtime.adapter, argv[i], sizeof(runtime.adapter));
        else if (!strcmp(option, "--stage-version")) version = argv[i];
        else if (!strcmp(option, "--test-serial")) test_serial = argv[i];
        else if (!strcmp(option, "--maintenance-lock-fd")) {
            char *end = NULL; long fd = strtol(argv[i], &end, 10);
            if (!*argv[i] || !end || *end || fd < 0 || fd > INT_MAX) return 2;
            maintenance_lock_fd = (int)fd;
        }
        else { sl_log("Unknown option: %s\n", option); return 2; }
    }
    sl_debug_enable(debug);
    sl_debug("enabled; session states: 0=idle 1=AUTH0 2=AUTH1; payloads and credentials omitted");
    if (!*runtime.release || runtime.release[0] != '/' || (test_serial && !session_bus) || (allow_create && !initialize) || (initialize && !*version)) {
        sl_log("Invalid arguments; use --help\n"); return 2;
    }
    if ((reset || reset_config_only || notify_reset || authorize_reset || resume_unbind) && (geteuid() != 0 || initialize || check || stdio_mode || session_bus ||
        *runtime.config || *version || (reset + reset_config_only + notify_reset + authorize_reset + resume_unbind > 1))) {
        sl_log("Reset requires root and exactly one reset mode, without identity overrides or test modes (effective_uid=%lu)\n", (unsigned long)geteuid());
        return 2;
    }
    if ((maintenance_lock_fd >= 0) != (reset_config_only || notify_reset || authorize_reset)) {
        sl_log("Internal reset operations require --maintenance-lock-fd; external reset modes must not supply it\n");
        return 2;
    }
    if (test_serial) {
        runtime.responder_id[0] = 1;
        if (!sl_unhex(test_serial, runtime.responder_id + 8, 8)) return 2;
    }
    if (!*runtime.config) {
        char *path = g_build_filename(runtime.release, "user", ".hoorii", "config.json", NULL);
        g_strlcpy(runtime.config, path, sizeof(runtime.config)); g_free(path);
    }
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return 1;
    setvbuf(stdout, NULL, _IOLBF, 0);
    int ok;
    gboolean internal_reset = authorize_reset || notify_reset || reset_config_only;
    int reset_lock_ok = !internal_reset || sl_reset_lock_held(maintenance_lock_fd);
    if (!reset_lock_ok)
        sl_debug("Reset maintenance lock validation failed: inherited FD must own the same exclusive host lock");
    if (resume_unbind) ok = sl_recover_unbind(&runtime);
    else if (reset) ok = sl_reset_services(&runtime, FALSE, NULL);
    else if (authorize_reset) ok = reset_lock_ok && sl_authorize_unbind(&runtime, "factory_reset");
    else if (notify_reset) ok = reset_lock_ok &&
        (sl_websocket_event("{\"event\":\"RESET\"}") || sl_websocket_event("{\"event\":\"RESET\"}"));
    else if (reset_config_only) ok = reset_lock_ok && sl_authorize_unbind(&runtime, "factory_reset") && sl_identity_load(&runtime) && sl_config_reset(&runtime);
    else if (initialize) ok = sl_identity_init(&runtime, version, allow_create);
    else {
        ok = sl_identity_load(&runtime);
        if (ok && !check) {
            if (stdio_mode) {
                char *line = NULL; size_t capacity = 0; ssize_t n;
                while ((n = getline(&line, &capacity, stdin)) >= 0) {
                    g_strstrip(line); size_t length = strlen(line) / 2;
                    uint8_t input[SL_MAX_FRAME], output[SL_MAX_FRAME]; size_t out_length = 0;
                    int handled = strlen(line) % 2 == 0 && length <= SL_MAX_FRAME && sl_unhex(line, input, length) &&
                        sl_handle_frame(&runtime, input, length, output, &out_length);
                    char *hex = handled ? sl_hex(output, out_length) : g_strdup("ERROR"); puts(hex); g_free(hex);
                    sl_after_response(&runtime, handled && out_length > 0);
                }
                free(line);
            } else ok = sl_ble_run(&runtime, session_bus);
        }
    }
    if (!ok) sl_log("Shadowlink operation failed; established identity was not replaced\n");
    OPENSSL_cleanse(runtime.private_key, sizeof(runtime.private_key)); sl_session_reset(&runtime.session);
    g_free(runtime.pending_userinfo); curl_global_cleanup();
    return ok ? 0 : 1;
}
