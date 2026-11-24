#include "shadowlink.h"
#include <string.h>
#include <openssl/evp.h>

static void append_bits(uint8_t *data, unsigned *offset, uint32_t value, unsigned bits)
{
    while (bits) {
        --bits;
        data[*offset / 8] |= (uint8_t)(((value >> bits) & 1) << (7 - *offset % 8));
        ++*offset;
    }
}

int sl_onboarding(uint16_t vendor, uint16_t product, uint16_t discriminator,
                   uint32_t passcode, const uint8_t responder_id[16],
                   char *out, size_t capacity)
{
    static const char alphabet[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-.";
    uint8_t data[32] = {0};
    unsigned offset = 0;
    size_t n = 3;
    if (discriminator > 4095 || !passcode || passcode > 99999998 || capacity < 58) return 0;
    for (uint32_t repeated = 11111111; repeated <= 88888888; repeated += 11111111)
        if (passcode == repeated) return 0;
    if (passcode == 12345678 || passcode == 87654321) return 0;
    append_bits(data, &offset, 0, 3);
    append_bits(data, &offset, vendor, 16);
    append_bits(data, &offset, product, 16);
    append_bits(data, &offset, 0, 10);
    append_bits(data, &offset, discriminator, 12);
    append_bits(data, &offset, passcode, 27);
    append_bits(data, &offset, 0, 4);
    data[11] = 0x15; data[12] = 0x28; data[13] = 1; data[14] = 16;
    memcpy(data + 15, responder_id, 16);
    data[31] = 0x18;
    memcpy(out, "SL:", 3);
    /* Existing Shadowlink uses one big-endian integer, unlike Matter's
     * independent little-endian three-byte base38 groups. */
    size_t first = 0;
    while (first < sizeof(data)) {
        unsigned remainder = 0;
        for (size_t i = first; i < sizeof(data); ++i) {
            unsigned value = remainder * 256 + data[i];
            data[i] = (uint8_t)(value / 38);
            remainder = value % 38;
        }
        out[n++] = alphabet[remainder];
        while (first < sizeof(data) && data[first] == 0) ++first;
    }
    for (size_t a = 3, b = n - 1; a < b; ++a, --b) {
        char tmp = out[a]; out[a] = out[b]; out[b] = tmp;
    }
    out[n] = '\0';
    return 1;
}

int sl_advertisement(uint16_t discriminator, int commissioned,
                     const uint8_t responder_id[16], const uint8_t public_key[65],
                     uint8_t out[10])
{
    static const char label[] = "ShadowLinkScanRspV0";
    uint8_t data[sizeof(label) - 1 + 16 + 65], digest[32];
    if (discriminator > 4095 || (commissioned != 0 && commissioned != 1)) return 0;
    uint16_t field = discriminator | (commissioned ? 0x8000 : 0);
    memset(out, 0, 10);
    out[0] = (uint8_t)field; out[1] = (uint8_t)(field >> 8);
    if (!commissioned) return 1;
    _Static_assert(sizeof(label) - 1 + 16 + 65 == sizeof(data), "advertisement input size");
    memcpy(data, label, sizeof(label) - 1);
    memcpy(data + sizeof(label) - 1, responder_id, 16);
    memcpy(data + sizeof(label) - 1 + 16, public_key, 65);
    if (!EVP_Digest(data, sizeof(data), digest, NULL, EVP_sha256(), NULL)) return 0;
    memcpy(out + 2, digest, 8);
    return 1;
}
