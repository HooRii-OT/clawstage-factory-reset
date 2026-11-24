#define _GNU_SOURCE
#include "runtime.h"
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <gio/gunixsocketaddress.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    GString *line;
    gboolean oversized, private_key;
} log_stream;

static void flush_log_line(log_stream *stream)
{
    const char *line = stream->line->str;
    gboolean begin = strstr(line, "-----BEGIN ") && strstr(line, "PRIVATE KEY-----");
    gboolean end = strstr(line, "-----END ") && strstr(line, "PRIVATE KEY-----");
    if (begin) { fputs("[REDACTED PRIVATE KEY]\n", stdout); stream->private_key = TRUE; }
    if (!stream->private_key && !stream->oversized) {
        char *safe = sl_log_redact(line);
        fputs(safe, stdout); g_free(safe);
    }
    if (end) stream->private_key = FALSE;
    fflush(stdout);
    g_string_truncate(stream->line, 0);
    stream->oversized = FALSE;
}

static void stream_log(log_stream *stream, const char *buffer, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        if (!stream->oversized) g_string_append_c(stream->line, buffer[i]);
        if (stream->line->len > 16384) {
            /* Never split a credential across independently sanitized chunks. */
            fputs("[log line omitted: exceeds 16 KiB]\n", stdout); fflush(stdout);
            g_string_truncate(stream->line, 0); stream->oversized = TRUE;
        }
        if (buffer[i] == '\n') {
            size_t n = stream->line->len;
            while (n && g_ascii_isspace(stream->line->str[n - 1])) --n;
            /* Retain a pretty-printed key until its value arrives. */
            if (!stream->oversized && n && (stream->line->str[n - 1] == ':' || stream->line->str[n - 1] == '=')) continue;
            flush_log_line(stream);
        }
    }
}

static int collect_output(GString *captured, log_stream *stream, const char *buffer, size_t length,
                          const char *marker, gboolean *seen)
{
    if (marker) {
        stream_log(stream, buffer, length);
        g_string_append_len(captured, buffer, (gssize)length);
        if (strstr(captured->str, marker)) *seen = TRUE;
        /* Keep only the boundary needed to recognize a split marker. Verbose
         * reset logs must neither exhaust memory nor hit the capture limit. */
        size_t keep = strlen(marker) - 1;
        if (captured->len > keep) g_string_erase(captured, 0, captured->len - keep);
        return 1;
    }
    if (captured->len + length > 1024 * 1024) return 0;
    g_string_append_len(captured, buffer, (gssize)length);
    return 1;
}

static int run(const char *const argv[], unsigned timeout, char **output, gboolean capture_errors,
               const char *marker, gboolean *seen)
{
    /* Arguments and child output may contain passwords; log only the tool. */
    sl_debug("exec tool=%s timeout=%us", argv[0], timeout);
    int pipes[2], status = 0, exited = 0, timed_out = 0, wait_error = 0;
    if (output) *output = NULL;
    if (pipe2(pipes, O_CLOEXEC)) {
        if (marker) sl_log("[FACTORY-RESET-ERROR] Cannot create log pipe: %s\n", strerror(errno));
        return 0;
    }
    pid_t pid = fork();
    if (pid < 0) {
        if (marker) sl_log("[FACTORY-RESET-ERROR] Cannot start lifecycle: %s\n", strerror(errno));
        close(pipes[0]); close(pipes[1]); return 0;
    }
    if (!pid) {
        setpgid(0, 0);
        int input = open("/dev/null", O_RDONLY);
        if (input >= 0) dup2(input, STDIN_FILENO);
        dup2(pipes[1], STDOUT_FILENO);
        /* Ordinary command output can contain credentials. Only the reset
         * lifecycle explicitly opts into streaming its diagnostic output. */
        int errors = capture_errors ? pipes[1] : open("/dev/null", O_WRONLY);
        if (errors >= 0) dup2(errors, STDERR_FILENO);
        close(pipes[0]); close(pipes[1]);
        execvp(argv[0], (char *const *)argv);
        if (marker) dprintf(STDERR_FILENO, "[FACTORY-RESET-ERROR] Cannot execute %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    setpgid(pid, pid);
    close(pipes[1]);
    fcntl(pipes[0], F_SETFL, O_NONBLOCK);
    GString *captured = g_string_new(marker ? "\n" : NULL);
    log_stream stream = {.line = g_string_new(NULL)};
    gint64 deadline = g_get_monotonic_time() + (gint64)timeout * G_USEC_PER_SEC;
    for (;;) {
        char buffer[4096]; ssize_t n;
        while ((n = read(pipes[0], buffer, sizeof(buffer))) > 0) {
            if (!collect_output(captured, &stream, buffer, (size_t)n, marker, seen)) goto terminate;
            if (g_get_monotonic_time() >= deadline) { timed_out = 1; goto terminate; }
        }
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) { exited = 1; break; }
        if (result < 0 && errno != EINTR) { wait_error = errno; goto terminate; }
        if (g_get_monotonic_time() >= deadline) { timed_out = 1; goto terminate; }
        struct pollfd pfd = {pipes[0], POLLIN, 0};
        poll(&pfd, 1, 25);
    }
    /* Collect the final bytes after the child exits. */
    for (;;) {
        char buffer[4096]; ssize_t n = read(pipes[0], buffer, sizeof(buffer));
        if (n <= 0) break;
        if (!collect_output(captured, &stream, buffer, (size_t)n, marker, seen)) { exited = 0; break; }
    }
    goto done;
 terminate:
    if (marker && timed_out) sl_log("[FACTORY-RESET-ERROR] Lifecycle timed out (timeout=%us); terminating reset process group\n", timeout);
    if (marker && wait_error) sl_log("[FACTORY-RESET-ERROR] Cannot wait for lifecycle: %s\n", strerror(wait_error));
    kill(-pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
 done:
    close(pipes[0]);
    if (marker && stream.line->len) flush_log_line(&stream);
    g_string_free(stream.line, TRUE);
    sl_debug("exec tool=%s completed=%d exit=%d signal=%d", argv[0], exited,
        WIFEXITED(status) ? WEXITSTATUS(status) : -1, WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    if (marker) sl_log("[FACTORY-RESET] Lifecycle completed=%d exit_code=%d signal=%d accepted=%d\n", exited,
        exited && WIFEXITED(status) ? WEXITSTATUS(status) : -1, WIFSIGNALED(status) ? WTERMSIG(status) : 0, *seen);
    if (output) *output = g_string_free(captured, FALSE); else g_string_free(captured, TRUE);
    return exited && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
int sl_run(const char *const argv[], unsigned timeout, char **output)
{
    return run(argv, timeout, output, FALSE, NULL, NULL);
}
int sl_run_with_stderr(const char *const argv[], unsigned timeout, char **output)
{
    return run(argv, timeout, output, TRUE, NULL, NULL);
}
int sl_run_logged(const char *const argv[], unsigned timeout, const char *marker, gboolean *seen)
{
    char *line = g_strdup_printf("\n%s\n", marker);
    *seen = FALSE;
    int ok = run(argv, timeout, NULL, TRUE, line, seen);
    g_free(line);
    return ok;
}
static size_t http_write(char *data, size_t size, size_t count, void *user)
{
    GString *buffer = user;
    if (count && size > SIZE_MAX / count) return 0;
    size_t n = size * count;
    if (n > 1024 * 1024 - buffer->len) return 0;
    g_string_append_len(buffer, data, (gssize)n);
    return n;
}
int sl_http(const char *url, const char *body, const char *authorization,
            unsigned timeout, char **response, long *status)
{
    CURL *curl = curl_easy_init();
    GString *buffer = g_string_new(NULL);
    struct curl_slist *headers = NULL;
    if (response) *response = NULL;
    *status = 0;
    if (!curl) { g_string_free(buffer, TRUE); return 0; }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeout);
    curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, buffer);
    if (body) {
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    }
    if (authorization && *authorization) {
        char *header = g_strconcat("Authorization: Bearer ", authorization, NULL);
        if (!strchr(header, '\r') && !strchr(header, '\n')) headers = curl_slist_append(headers, header);
        g_free(header);
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    CURLcode result = curl_easy_perform(curl);
    int ok = result == CURLE_OK;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status);
    sl_debug("HTTP method=%s transport=%d status=%ld response_bytes=%zu", body ? "POST" : "GET", (int)result, *status, (size_t)buffer->len);
    if (result != CURLE_OK) sl_debug("HTTP transport failure: %s", curl_easy_strerror(result));
    curl_slist_free_all(headers); curl_easy_cleanup(curl);
    if (response) *response = g_string_free(buffer, FALSE); else g_string_free(buffer, TRUE);
    return ok;
}
json_object *sl_update_call(const char *operation, json_object *arguments, char **code, char **error)
{
    if (strcmp(operation, "get_status") && strcmp(operation, "get_pending_status"))
        sl_debug("OTA controller operation=%s", operation);
    const char *path = g_getenv("SOFTWARE_UPDATE_SOCKET");
    if (!path) path = "/run/clawstage-software-update/control.sock";
    if (code) *code = NULL;
    if (error) *error = NULL;
    json_object *request = arguments ? json_object_get(arguments) : json_object_new_object();
    json_object_object_add(request, "operation", json_object_new_string(operation));
    GSocketClient *client = g_socket_client_new();
    g_socket_client_set_timeout(client, 3);
    GSocketAddress *address = g_unix_socket_address_new(path);
    GSocketConnection *connection = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(address), NULL, NULL);
    json_object *result = NULL, *reply = NULL;
    const char *failure_code = "SOFTWARE_UPDATE_SERVICE_UNAVAILABLE", *failure = "Software update service unavailable";
    GString *wire = g_string_new(json_object_to_json_string_ext(request, JSON_C_TO_STRING_PLAIN));
    g_string_append_c(wire, '\n');
    if (!connection) goto done;
    if (!g_output_stream_write_all(g_io_stream_get_output_stream(G_IO_STREAM(connection)), wire->str, wire->len, NULL, NULL, NULL)) goto done;
    g_string_truncate(wire, 0);
    GInputStream *input = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    gint64 deadline = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
    while (wire->len < 1024 * 1024 && g_get_monotonic_time() < deadline) {
        char buffer[4096];
        gssize n = g_input_stream_read(input, buffer, sizeof(buffer), NULL, NULL);
        if (n <= 0) break;
        g_string_append_len(wire, buffer, n);
        if (memchr(buffer, '\n', (size_t)n)) break;
    }
    reply = sl_json_parse(wire->str, wire->len);
    failure_code = "SOFTWARE_UPDATE_INVALID_RESPONSE"; failure = "Invalid software update response";
    if (!json_object_is_type(reply, json_type_object)) goto done;
    json_object *ok = NULL;
    if (!json_object_object_get_ex(reply, "ok", &ok) || !json_object_get_boolean(ok)) {
        failure_code = sl_string(reply, "error_code"); failure = sl_string(reply, "error_message");
        goto done;
    }
    json_object_object_get_ex(reply, "result", &result);
    result = json_object_get(result);
    failure_code = failure = NULL;
 done:
    if (failure_code) sl_debug("OTA controller operation=%s failed code=%s", operation, failure_code);
    if (failure_code && code) *code = g_strdup(*failure_code ? failure_code : "SOFTWARE_UPDATE_ERROR");
    if (failure && error) *error = g_strdup(failure);
    json_object_put(reply); json_object_put(request); g_string_free(wire, TRUE);
    if (connection) { g_io_stream_close(G_IO_STREAM(connection), NULL, NULL); g_object_unref(connection); }
    g_object_unref(address); g_object_unref(client);
    return result;
}
int sl_websocket_send(const char *url, const char *json, double timeout)
{
    GUri *uri = g_uri_parse(url, G_URI_FLAGS_ENCODED, NULL);
    if (!uri) return 0;
    const char *scheme = g_uri_get_scheme(uri), *host = g_uri_get_host(uri);
    if (!scheme || strcmp(scheme, "ws") || !host || g_uri_get_userinfo(uri) ||
        !isfinite(timeout) || timeout <= 0 || timeout > 60) { g_uri_unref(uri); return 0; }
    int port = g_uri_get_port(uri); if (port < 0) port = 80;
    uint8_t random[16], digest[20], mask[4];
    if (strlen(json) > 65535 || RAND_bytes(random, 16) != 1 || RAND_bytes(mask, 4) != 1) { g_uri_unref(uri); return 0; }
    char *key = g_base64_encode(random, 16);
    char *challenge = g_strconcat(key, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", NULL);
    if (!EVP_Digest(challenge, strlen(challenge), digest, NULL, EVP_sha1(), NULL)) { g_free(key); g_free(challenge); g_uri_unref(uri); return 0; }
    char *expected = g_base64_encode(digest, sizeof(digest)); g_free(challenge);
    GSocketClient *client = g_socket_client_new(); g_socket_client_set_timeout(client, (guint)ceil(timeout));
    GSocketConnection *connection = g_socket_client_connect_to_host(client, host, (guint16)port, NULL, NULL);
    int ok = 0;
    GString *request = g_string_new(NULL), *reply = g_string_new(NULL);
    g_string_printf(request, "GET %s%s%s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n", *g_uri_get_path(uri) ? g_uri_get_path(uri) : "/", g_uri_get_query(uri) ? "?" : "", g_uri_get_query(uri) ? g_uri_get_query(uri) : "", host, port, key);
    if (!connection) goto done;
    GOutputStream *output = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    GInputStream *input = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    if (!g_output_stream_write_all(output, request->str, request->len, NULL, NULL, NULL)) goto done;
    gint64 deadline = g_get_monotonic_time() + (gint64)(timeout * G_USEC_PER_SEC);
    while (reply->len < 8192 && !g_str_has_suffix(reply->str, "\r\n\r\n") && g_get_monotonic_time() < deadline) {
        char ch;
        if (g_input_stream_read(input, &ch, 1, NULL, NULL) != 1) goto done;
        g_string_append_c(reply, ch);
    }
    if (!g_str_has_prefix(reply->str, "HTTP/1.1 101 ") || !g_str_has_suffix(reply->str, "\r\n\r\n")) goto done;
    gboolean accepted = FALSE;
    char **lines = g_strsplit(reply->str, "\r\n", -1);
    for (size_t i = 0; lines[i]; ++i) {
        char *colon = strchr(lines[i], ':');
        if (!colon) continue;
        *colon = 0;
        if (!g_ascii_strcasecmp(lines[i], "Sec-WebSocket-Accept") && !strcmp(g_strstrip(colon + 1), expected)) accepted = TRUE;
    }
    g_strfreev(lines);
    if (!accepted) goto done;
    size_t n = strlen(json);
    g_string_truncate(request, 0); g_string_append_c(request, (char)0x81);
    if (n < 126) g_string_append_c(request, (char)(0x80 | n));
    else { g_string_append_c(request, (char)0xfe); g_string_append_c(request, (char)(n >> 8)); g_string_append_c(request, (char)n); }
    g_string_append_len(request, (const char *)mask, 4);
    for (size_t i = 0; i < n; ++i) g_string_append_c(request, (char)((uint8_t)json[i] ^ mask[i % 4]));
    const uint8_t close_frame[] = {0x88, 0x80, 0, 0, 0, 0};
    g_string_append_len(request, (const char *)close_frame, sizeof(close_frame));
    ok = g_output_stream_write_all(output, request->str, request->len, NULL, NULL, NULL);
 done:
    if (connection) { g_io_stream_close(G_IO_STREAM(connection), NULL, NULL); g_object_unref(connection); }
    sl_debug("WebSocket result=%d json_bytes=%zu (not a receiver acknowledgement)", ok, strlen(json));
    g_object_unref(client); g_free(key); g_free(expected); g_string_free(request, TRUE); g_string_free(reply, TRUE); g_uri_unref(uri);
    return ok;
}

int sl_websocket_event(const char *json)
{
    return sl_websocket_send("ws://127.0.0.1:9001/ws", json, 5);
}
