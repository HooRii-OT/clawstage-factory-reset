#define _GNU_SOURCE
#include "device.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

/* Run the actual native dispatcher against a harmless lifecycle fixture.
 * A file handshake proves output reaches the caller BEFORE cleanup finishes. */
static char *exercise(const char *body, int success, gboolean accepted, gboolean handshake,
                      gboolean timeout)
{
    char directory[] = "/tmp/shadowlink-reset-log-XXXXXX";
    CHECK(mkdtemp(directory));
    char *script = g_build_filename(directory, "factory-reset.sh", NULL);
    char *gate = g_build_filename(directory, "continue", NULL);
    char *source = g_strdup_printf("#!/bin/bash\nset -eu\ncd '%s'\n%s", directory, body);
    CHECK(g_file_set_contents(script, source, -1, NULL));
    g_free(source);
    int pipes[2];
    CHECK(pipe(pipes) == 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        close(pipes[0]);
        CHECK(dup2(pipes[1], STDOUT_FILENO) >= 0);
        CHECK(dup2(pipes[1], STDERR_FILENO) >= 0);
        close(pipes[1]);
        gboolean started = FALSE;
        int ok;
        if (timeout) {
            const char *command[] = {"/bin/bash", script, NULL};
            ok = sl_run_logged(command, 1, "CLAWSTAGE_RESET_ACCEPTED", &started);
        } else {
            sl_runtime runtime = {0};
            g_strlcpy(runtime.release, directory, sizeof(runtime.release));
            ok = sl_reset_services(&runtime, FALSE, &started);
        }
        fflush(NULL);
        _exit(ok == success && started == accepted ? 0 : 1);
    }
    close(pipes[1]);
    CHECK(fcntl(pipes[0], F_SETFL, O_NONBLOCK) == 0);
    GString *output = g_string_new(NULL);
    gboolean released = FALSE, finished = FALSE;
    gint64 deadline = g_get_monotonic_time() + 8 * G_USEC_PER_SEC;
    while (!finished && g_get_monotonic_time() < deadline) {
        struct pollfd descriptor = {pipes[0], POLLIN, 0};
        poll(&descriptor, 1, 20);
        char buffer[8192];
        ssize_t count = read(pipes[0], buffer, sizeof(buffer));
        if (count > 0) g_string_append_len(output, buffer, count);
        else if (!count) finished = TRUE;
        else CHECK(errno == EINTR || errno == EAGAIN);
        if (handshake && !released && strstr(output->str, "stdout-progress\n") &&
            strstr(output->str, "stderr-progress\n")) {
            int status;
            CHECK(waitpid(child, &status, WNOHANG) == 0);
            CHECK(g_file_set_contents(gate, "go", -1, NULL));
            released = TRUE;
        }
    }
    close(pipes[0]);
    if (!finished) kill(child, SIGKILL);
    int status;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(finished && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(!handshake || released);
    unlink(gate);
    CHECK(unlink(script) == 0 && rmdir(directory) == 0);
    g_free(gate); g_free(script);
    return g_string_free(output, FALSE);
}

int main(void)
{
    char *output = exercise(
        "printf 'stdout-progress\\n'\nprintf 'stderr-progress\\n' >&2\n"
        "for ((i=0;i<300;i++)); do [ ! -f continue ] || break; sleep 0.01; done\n"
        "[ -f continue ]\n"
        /* More than the old 1 MiB capture limit, with a split marker afterward. */
        "head -c 1100000 /dev/zero | tr '\\0' 'x'\nprintf '\\nCLAWSTAGE_RESET_'\n"
        "sleep 0.05\nprintf 'ACCEPTED\\nlast-output\\n'\n",
        1, TRUE, TRUE, FALSE);
    CHECK(strlen(output) < 16384 && strstr(output, "last-output\n"));
    CHECK(strstr(output, "[log line omitted: exceeds 16 KiB]"));
    CHECK(strstr(output, "exit_code=0 signal=0 accepted=1"));
    g_free(output);

    output = exercise(
        "printf 'ssid=OfficeWiFi password='\nsleep 0.05\nprintf '\"wifi-secret with spaces\"\\n'\n"
        "printf '\"ha_token\":\\n  \"ha-secret\"\\n' >&2\n"
        "printf '%s\\n' '-----BEGIN PRIVATE KEY-----' 'private-key-secret' '-----END PRIVATE KEY-----'\n"
        "printf 'requestId=diagnostic-id endpoint=http://127.0.0.1:18789/path\\n'\n"
        "printf 'CLAWSTAGE_RESET_ACCEPTED\\n'\n", 1, TRUE, FALSE, FALSE);
    CHECK(strstr(output, "ssid=OfficeWiFi") && strstr(output, "requestId=diagnostic-id"));
    CHECK(strstr(output, "endpoint=http://127.0.0.1:18789/path"));
    CHECK(strstr(output, "[REDACTED]") && strstr(output, "[REDACTED PRIVATE KEY]"));
    CHECK(!strstr(output, "wifi-secret") && !strstr(output, "ha-secret") && !strstr(output, "private-key-secret"));
    g_free(output);

    output = exercise("printf 'CLAWSTAGE_RESET_ACCEPTED\\n'\nprintf 'specific failure reason\\n' >&2\nexit 41\n",
                      0, TRUE, FALSE, FALSE);
    CHECK(strstr(output, "specific failure reason\n") && strstr(output, "exit_code=41"));
    g_free(output);

    output = exercise("printf 'prefixCLAWSTAGE_RESET_ACCEPTED\\nCLAWSTAGE_RESET_ACCEPTEDsuffix\\n'\n",
                      0, FALSE, FALSE, FALSE);
    CHECK(strstr(output, "Reset was not accepted"));
    g_free(output);

    output = exercise("printf 'CLAWSTAGE_RESET_ACCEPTED\\n'\nkill -TERM $$\n",
                      0, TRUE, FALSE, FALSE);
    CHECK(strstr(output, "signal=15 accepted=1"));
    g_free(output);

    output = exercise("printf 'CLAWSTAGE_RESET_ACCEPTED\\n'\nsleep 10\n",
                      0, TRUE, FALSE, TRUE);
    CHECK(strstr(output, "timeout=1s") && strstr(output, "completed=0"));
    g_free(output);
    return 0;
}
