#include "device.h"
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static void panel(const char *root, const char *name, gboolean create)
{
    char *directory = g_build_filename(root, name, NULL);
    char *brightness = g_build_filename(directory, "brightness", NULL);
    char *maximum = g_build_filename(directory, "max_brightness", NULL);
    if (create) {
        CHECK(g_mkdir(directory, 0700) == 0);
        CHECK(g_file_set_contents(brightness, "128\n", -1, NULL));
        CHECK(g_file_set_contents(maximum, "254\n", -1, NULL));
    } else {
        CHECK(g_remove(brightness) == 0);
        CHECK(g_remove(maximum) == 0);
        CHECK(g_rmdir(directory) == 0);
    }
    g_free(directory); g_free(brightness); g_free(maximum);
}

int main(void)
{
    char *root = g_dir_make_tmp("shadowlink-backlight-XXXXXX", NULL);
    CHECK(root);
    CHECK(sl_backlight_at(root, -1) == -1);
    panel(root, "xy040bhd_backlight", TRUE);
    CHECK(sl_backlight_at(root, -1) == 50);
    CHECK(sl_backlight_at(root, 60) == 60);
    CHECK(sl_backlight_at(root, -1) == 60);
    CHECK(sl_backlight_at(root, 101) == -1);
    CHECK(sl_backlight_at(root, -1) == 60);
    panel(root, "other-panel", TRUE);
    CHECK(sl_backlight_at(root, 70) == -1);
    panel(root, "fl7703n_backlight", TRUE);
    CHECK(sl_backlight_at(root, -1) == 50);
    CHECK(sl_backlight_at(root, 0) == 0);
    CHECK(sl_backlight_at(root, -1) == 0);
    panel(root, "fl7703n_backlight", FALSE);
    panel(root, "other-panel", FALSE);
    CHECK(sl_backlight_at(root, -1) == 60);
    panel(root, "xy040bhd_backlight", FALSE);
    panel(root, "rpi-dsi-display-bl", TRUE);
    CHECK(sl_backlight_at(root, 100) == 100);
    CHECK(sl_backlight_at(root, -1) == 100);
    panel(root, "rpi-dsi-display-bl", FALSE);
    CHECK(g_rmdir(root) == 0);
    g_free(root);
    return 0;
}
