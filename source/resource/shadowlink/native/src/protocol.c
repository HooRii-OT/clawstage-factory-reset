#include "shadowlink.h"
#include <string.h>

int sl_tlv_next(const uint8_t *data, size_t length, size_t *offset, sl_tlv *entry)
{
    size_t p = *offset, n;
    uint16_t tag;
    if (p == length) return 0;
    if (p > length || length - p < 2) return -1;
    tag = data[p++];
    if (tag == 0x7f) {
        if (length - p < 2) return -1;
        tag = (uint16_t)((tag << 8) | data[p++]);
    }
    n = data[p++];
    if (n == 0x81) {
        if (length - p < 1) return -1;
        n = data[p++];
    } else if (n == 0x82) {
        if (length - p < 2) return -1;
        n = ((size_t)data[p] << 8) | data[p + 1];
        p += 2;
    } else if (n > 0x7f) {
        return -1;
    }
    if (n > length - p) return -1;
    entry->tag = tag;
    entry->value = data + p;
    entry->length = n;
    *offset = p + n;
    return 1;
}

int sl_tlv_put(uint8_t *out, size_t capacity, size_t *length,
               uint16_t tag, const uint8_t *value, size_t n)
{
    size_t p = *length;
    size_t header = (tag > 255 ? 2 : 1) + (n < 128 ? 1 : n < 256 ? 2 : 3);
    if ((tag > 255 && (tag >> 8) != 0x7f) || tag == 0x7f || n > 65535 ||
        p > capacity || header > capacity - p || n > capacity - p - header)
        return 0;
    if (tag > 255) out[p++] = 0x7f;
    out[p++] = (uint8_t)tag;
    if (n >= 256) { out[p++] = 0x82; out[p++] = (uint8_t)(n >> 8); }
    else if (n >= 128) out[p++] = 0x81;
    out[p++] = (uint8_t)n;
    if (n) memcpy(out + p, value, n);
    *length = p + n;
    return 1;
}

int sl_receive(sl_receiver *rx, const uint8_t *data, size_t length,
               size_t offset, uint64_t now, sl_frame_callback callback, void *user)
{
    if (rx->length && (now < rx->last_ms || now - rx->last_ms > 10000))
        rx->length = 0;
    if (offset && offset != rx->length) goto fail;
    rx->last_ms = now;
    while (length) {
        size_t target = 4, take;
        if (rx->length >= 4) {
            target += ((size_t)rx->data[2] << 8) | rx->data[3];
            if (target > SL_MAX_FRAME) goto fail;
        }
        take = target - rx->length;
        if (take > length) take = length;
        memcpy(rx->data + rx->length, data, take);
        rx->length += take;
        data += take;
        length -= take;
        if (rx->length >= 4) {
            target = 4 + (((size_t)rx->data[2] << 8) | rx->data[3]);
            if (target > SL_MAX_FRAME) goto fail;
            if (rx->length == target) {
                rx->length = 0;
                if (!callback(rx->data, target, user)) goto fail;
            }
        }
    }
    return 1;
fail:
    rx->length = 0;
    return 0;
}
