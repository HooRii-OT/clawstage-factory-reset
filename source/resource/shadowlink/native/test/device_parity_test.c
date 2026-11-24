/* Device helpers with a fake nmcli; no network/audio changes are performed. */
#include "device.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static int listings, activated, refreshed, disabled_late, restarted;
static gboolean storage_mode;
static const char *failure_reason;
static unsigned statvfs_calls;

/* Real disk usage can change between the assertion and sl_storage(), especially
 * while Docker writes build output. Keep this parity check independent of the
 * host filesystem, with different values for free/available and size/frsize. */
int __wrap_statvfs(const char *path, struct statvfs *fs)
{
    CHECK(storage_mode && !strcmp(path, "/"));
    ++statvfs_calls;
    memset(fs, 0, sizeof(*fs));
    fs->f_blocks = 100;
    fs->f_bfree = 40;
    fs->f_bavail = 30;
    fs->f_frsize = 4096;
    fs->f_bsize = 8192;
    return 0;
}

void __wrap_g_usleep(gulong usec) { (void)usec; }
int __wrap_sl_run(const char *const argv[], unsigned timeout, char **output)
{
    (void)timeout;
    if (output) *output = NULL;
    if (!strcmp(argv[0], "lsblk")) {
        CHECK(storage_mode);
        *output = g_strdup("{\"blockdevices\":[{\"name\":\"mmcblk0\",\"type\":\"disk\",\"size\":1048576,\"mountpoints\":\"/\",\"children\":[{\"mountpoints\":[\"/\"]}]}]}");
        return 1;
    }
    if (!strcmp(argv[0], "docker")) { CHECK(disabled_late); restarted++; return 1; }
    CHECK(!strcmp(argv[0], "nmcli"));
    if (!strcmp(argv[1], "--escape") && !strcmp(argv[3], "-t")) {
        ++listings;
        *output = g_strdup(listings == 1 ? "preferred:wifi:shadowlink-wifi-preferred\n" :
            "preferred:wifi:shadowlink-wifi-preferred\nlate:wifi:added-after-activation\n");
    } else if (!strcmp(argv[1], "--escape")) *output = g_strdup("test-ssid\n");
    else if (!strcmp(argv[1], "-g")) {
        CHECK(strcmp(argv[2], "GENERAL.REASON"));
        if (!strcmp(argv[2], "IP4.ADDRESS")) *output = g_strdup(" 192.0.2.10/24\n192.0.2.11/24\n");
        else *output = g_strdup("");
    } else if (!strcmp(argv[1], "connection") && !strcmp(argv[2], "up")) {
        if (failure_reason) return 0;
        activated++;
    }
    else if (!strcmp(argv[1], "connection") && !strcmp(argv[2], "modify")) {
        if (activated && !strcmp(argv[4], "preferred")) refreshed++;
        if (!strcmp(argv[4], "late")) { CHECK(refreshed); disabled_late++; }
    }
    return 1;
}
int __wrap_sl_run_with_stderr(const char *const argv[], unsigned timeout, char **output)
{
    CHECK(!strcmp(argv[0], "env") && !strcmp(argv[1], "LC_ALL=C"));
    CHECK(!strcmp(argv[2], "nmcli") && !strcmp(argv[4], "up") && timeout == 30);
    int result = __wrap_sl_run(argv + 2, timeout, output);
    *output = g_strdup(failure_reason ? failure_reason : "Connection activated");
    return result;
}
int __real_sl_run_with_stderr(const char *const argv[], unsigned timeout, char **output);
int main(void)
{
    char *output = NULL;
    CHECK(!__real_sl_run_with_stderr((const char *const[]){"sh", "-c",
        "printf stdout; printf stderr >&2; exit 4", NULL}, 2, &output));
    CHECK(strstr(output, "stdout") && strstr(output, "stderr")); g_free(output);
    uint8_t ip[4];
    CHECK(sl_wifi_connect("test-ssid", "", ip));
    CHECK(listings == 2 && activated == 1 && refreshed == 1 && disabled_late == 1 && restarted == 1);
    CHECK(!memcmp(ip, (uint8_t[]){192, 0, 2, 10}, 4));
    const char *reasons[] = {
        "Error: Connection activation failed: (53) The Wi-Fi network could not be found.\n",
        "Error: Connection activation failed: Secrets were required, but not provided.",
        "", "Error: Timeout expired."};
    for (size_t i = 0; i < G_N_ELEMENTS(reasons); ++i) {
        failure_reason = reasons[i];
        CHECK(sl_wifi_connect("test-ssid", "", ip) == (i == 0 ? SL_WIFI_NOT_FOUND : 0));
        CHECK(activated == 1 && disabled_late == 1 && restarted == 1);
    }
    storage_mode = TRUE;
    uint64_t usage[4];
    sl_storage(usage);
    CHECK(statvfs_calls == 2); /* Both parent and child report the same mount. */
    CHECK(usage[0] == 245760); /* (100 - 40) * 4096, counted only once. */
    CHECK(usage[1] == 1048576 && !usage[2] && !usage[3]);
    return 0;
}
