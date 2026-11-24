#define _GNU_SOURCE
#include "device.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

int main(int argc, char **argv)
{
    if (argc == 2) return sl_reset_lock_held(atoi(argv[1])) ? 0 : 1;
    char path[] = "/tmp/shadowlink-lock-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    CHECK(setenv("CLAWSTAGE_MAINTENANCE_LOCK", path, 1) == 0);
    CHECK(!sl_reset_lock_held(fd));
    CHECK(flock(fd, LOCK_EX | LOCK_NB) == 0);
    CHECK(sl_reset_lock_held(fd));
    int other = open(path, O_RDWR);
    CHECK(other >= 0 && !sl_reset_lock_held(other));
    close(other);
    CHECK(fchmod(fd, 0666) == 0 && !sl_reset_lock_held(fd));
    CHECK(fchmod(fd, 0600) == 0);
    /* Exercise the same fork/exec FD inheritance used by the shell lifecycle. */
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        char number[24]; snprintf(number, sizeof(number), "%d", fd);
        const char *emulator = getenv("SHADOWLINK_TEST_EMULATOR");
        if (emulator && *emulator)
            execlp(emulator, emulator, "-L", "/", argv[0], number, (char *)NULL);
        else
            execl(argv[0], argv[0], number, (char *)NULL);
        _exit(2);
    }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(unlink(path) == 0);
    CHECK(!sl_reset_lock_held(fd));
    CHECK(symlink("/dev/null", path) == 0 && !sl_reset_lock_held(fd));
    CHECK(unlink(path) == 0);
    close(fd);
    int shared = sl_binding_shared_lock();
    CHECK(shared >= 0);
    CHECK(sl_binding_lock() == -1);
    other = sl_binding_shared_lock();
    CHECK(other >= 0);
    close(other); close(shared);
    int exclusive = sl_binding_lock();
    CHECK(exclusive >= 0 && sl_binding_shared_lock() == -1);
    close(exclusive);
    CHECK(unlink(path) == 0);
    return 0;
}
