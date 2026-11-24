#define _GNU_SOURCE
#include "runtime.h"
#include <stdarg.h>
#include <stdio.h>

static gboolean debug_enabled = TRUE;

/* Redact credential values, not identifiers, paths, SSIDs or error context.
 * Call sites must still never log raw packets, keys or command arguments. */
char *sl_log_redact(const char *message)
{
    static gsize initialized;
    static GRegex *patterns[5];
    static const char *const expressions[] = {
        "-----BEGIN (?:[A-Z0-9]+ )*PRIVATE KEY-----[\\s\\S]*?(?:-----END (?:[A-Z0-9]+ )*PRIVATE KEY-----|$)",
        "(?i)((?<![a-z0-9_])(?:(?:wifi[-_ .]?|ssid[-_ .]?)?password|passwd|pwd|psk|passphrase|passcode|pin|api[-_ ]?key|"
        "(?:access|refresh|id|ha|hub|auth|bearer)[-_ ]?token|token|long_live_token|"
        "(?:long_term_)?private_key|kpersistent|incoming_key|outgoing_key|session_key|secret_key|client_secret|secret|"
        "authorization|cookie|set-cookie)[\\\"']?\\s*[:=]\\s*)"
        "(?:\\\"(?:\\\\.|[^\\\"\\\\])*\\\"|'(?:\\\\.|[^'\\\\])*'|[^\\r\\n]+?(?=\\s+[a-z_][a-z0-9_.-]*\\s*=|\\r?\\n|$))",
        "(?i)(\\b(?:Bearer|Basic)\\s+)[a-z0-9+/_.=~-]+",
        "(?i)((?:--)(?:password|passwd|psk|passphrase|passcode|api-key|token|secret)\\s+)"
        "(?:\\\"(?:\\\\.|[^\\\"\\\\])*\\\"|'(?:\\\\.|[^'\\\\])*'|[^\\s]+)",
        "(?i)([a-z][a-z0-9+.-]*://[^\\s/@:]+:)[^\\s/@]+(@)"
    };
    static const char *const replacements[] = {
        "[REDACTED PRIVATE KEY]", "\\1[REDACTED]", "\\1[REDACTED]", "\\1[REDACTED]", "\\1[REDACTED]\\2"
    };
    if (g_once_init_enter(&initialized)) {
        for (size_t i = 0; i < G_N_ELEMENTS(patterns); ++i)
            patterns[i] = g_regex_new(expressions[i], G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
        g_once_init_leave(&initialized, 1);
    }
    char *safe = g_strdup(message);
    for (size_t i = 0; i < G_N_ELEMENTS(patterns); ++i) {
        char *next = patterns[i] ? g_regex_replace(patterns[i], safe, -1, 0, replacements[i], 0, NULL) : NULL;
        g_free(safe);
        /* Fail closed if a future regex edit is invalid. */
        if (!next) return g_strdup("[log content hidden: redaction failed]\n");
        safe = next;
    }
    return safe;
}

static void write_log(const char *prefix, const char *format, va_list args, gboolean newline)
{
    char *message = g_strdup_vprintf(format, args);
    char *safe = sl_log_redact(message);
    flockfile(stderr);
    fputs(prefix, stderr);
    fputs(safe, stderr);
    if (newline) fputc('\n', stderr);
    funlockfile(stderr);
    g_free(safe); g_free(message);
}

void sl_debug_enable(gboolean enabled) { debug_enabled = enabled; }

void sl_debug(const char *format, ...)
{
    if (!debug_enabled) return;
    va_list args;
    va_start(args, format);
    write_log("[Shadowlink DEBUG] ", format, args, TRUE);
    va_end(args);
}

void sl_log(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    write_log("", format, args, FALSE);
    va_end(args);
}
