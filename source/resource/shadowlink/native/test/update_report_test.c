#include "runtime.h"
#include "constants.h"
#include <assert.h>
#include <string.h>

static void test_download(int mode, int legacy) {
    json_object *event = json_tokener_parse(
        "{\"status\":\"DOWNLOADING\",\"sequence\":9,\"transaction_id\":\"tx\","
        "\"download\":{\"downloaded_bytes\":5000000000,"
        "\"total_bytes\":10000000000,\"percent\":50}}");
    json_object *download = NULL;
    json_object_object_get_ex(event, "download", &download);
    if (mode == 1) { /* Unknown length: bytes only. */
        json_object_object_add(download, "total_bytes", NULL);
        json_object_object_add(download, "percent", NULL);
    } else if (mode == 2) /* Old controller: unchanged phase-only frame. */
        json_object_object_del(event, "download");
    else if (mode == 3)
        json_object_object_add(event, "status", json_object_new_string("VERIFYING"));
    uint8_t output[SL_MAX_FRAME];
    size_t length = 0, offset = 0;
    assert(sl_update_report(event, "hub", legacy ? "session" : NULL, output, &length));
    sl_tlv entry;
    int found = 0;
    while (sl_tlv_next(output + 4, length - 4, &offset, &entry) > 0) {
        if (entry.tag == TAG_DOWNLOAD_BYTES || entry.tag == TAG_DOWNLOAD_TOTAL_BYTES) {
            assert(entry.length == 8);
            uint64_t value = 0;
            for (size_t i = 0; i < entry.length; i++) value = (value << 8) | entry.value[i];
            assert(value == (entry.tag == TAG_DOWNLOAD_BYTES ? 5000000000ULL : 10000000000ULL));
            found++;
        } else if (entry.tag == TAG_DOWNLOAD_PERCENT) {
            assert(entry.length == 1 && entry.value[0] == 50);
            found++;
        } else if (entry.tag == TAG_OTA_PROTOCOL_VERSION)
            assert(entry.length == 1 && entry.value[0] == 3);
    }
    assert(found == (mode == 0 ? 3 : mode == 1 ? 1 : 0));
    json_object_put(event);
}

int main(void) {
    for (int legacy = 0; legacy < 2; legacy++)
        for (int mode = 0; mode < 4; mode++) test_download(mode, legacy);
    const char *names[] = {"REBOOT_PENDING", "REBOOTING_STAGE"};
    for (int legacy = 0; legacy < 2; ++legacy) {
        uint8_t previous[SL_MAX_FRAME];
        size_t previous_length = 0;
        for (size_t i = 0; i < G_N_ELEMENTS(names); ++i) {
            json_object *event = json_object_new_object();
            json_object_object_add(event, "status", json_object_new_string(names[i]));
            json_object_object_add(event, "sequence", json_object_new_int(8));
            json_object_object_add(event, "transaction_id", json_object_new_string("tx"));
            uint8_t output[SL_MAX_FRAME];
            size_t length = 0;
            assert(sl_update_report(event, "hub", legacy ? "session" : NULL, output, &length));
            size_t offset = 0;
            sl_tlv entry;
            int found = 0;
            while (sl_tlv_next(output + 4, length - 4, &offset, &entry) > 0) {
                if (entry.tag == TAG_UPDATE_STATUS) {
                    assert(entry.length == 1 && entry.value[0] == 0x14);
                    found++;
                }
            }
            assert(found == 1);
            if (i) assert(length == previous_length && !memcmp(output, previous, length));
            memcpy(previous, output, length);
            previous_length = length;
            json_object_put(event);
        }
    }
    return 0;
}
