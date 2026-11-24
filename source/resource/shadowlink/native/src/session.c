#include "shadowlink.h"
#include <string.h>
#include <openssl/crypto.h>

static const uint8_t initiator_longterm_x[32] = {
    0x54, 0xa3, 0xdf, 0x47, 0xda, 0x7e, 0xf6, 0x65,
    0xc4, 0xcb, 0x98, 0x2c, 0xf1, 0x20, 0x41, 0x2d,
    0x8d, 0x6b, 0x49, 0x3c, 0xb1, 0x23, 0xa3, 0xdd,
    0xb0, 0xee, 0x3b, 0x3b, 0xdc, 0x3f, 0x5c, 0x1b
};

void sl_session_reset(sl_session *session)
{
    OPENSSL_cleanse(session, sizeof(*session));
}

static void nonce_for(uint32_t counter, uint8_t nonce[12])
{
    memset(nonce, 0, 12);
    nonce[7] = 1;
    nonce[10] = (uint8_t)(counter >> 8);
    nonce[11] = (uint8_t)counter;
}

int sl_auth0(sl_session *s, const uint8_t *payload, size_t length,
             const uint8_t responder_id[16], uint8_t *out, size_t capacity, size_t *out_length)
{
    size_t offset = 0, n = 4;
    sl_tlv entry;
    unsigned seen = 0;
    int status;
    sl_session_reset(s);
    *out_length = 0;
    s->version[0] = 1;
    while ((status = sl_tlv_next(payload, length, &offset, &entry)) > 0) {
        unsigned bit = 0;
        switch (entry.tag) {
        case 0x87:
            if (entry.length != 65 || entry.value[0] != 4) goto fail;
            memcpy(s->initiator_public, entry.value, 65); bit = 1; break;
        case 0x4c:
            if (entry.length != 16) goto fail;
            memcpy(s->transaction, entry.value, 16); bit = 2; break;
        case 0x4d:
            if (entry.length != 16) goto fail;
            memcpy(s->initiator_id, entry.value, 16); bit = 4; break;
        case 0x5c:
            if (entry.length != 2) goto fail;
            memcpy(s->version, entry.value, 2); bit = 8; break;
        case 0x41:
            if (entry.length != 1) goto fail;
            s->flags = entry.value[0]; bit = 16; break;
        default: break;
        }
        if (seen & bit) goto fail;
        seen |= bit;
    }
    if (status < 0 || (seen & 7) != 7 || capacity < 91 ||
        !sl_keygen(s->ephemeral_private, s->ephemeral_public)) goto fail;
    if (!sl_tlv_put(out, capacity - 2, &n, 0x86, s->ephemeral_public, 65) ||
        !sl_tlv_put(out, capacity - 2, &n, 0x4f, responder_id, 16)) goto fail;
    out[0] = 0; out[1] = 1; out[2] = 0; out[3] = (uint8_t)(n - 4);
    out[n++] = 0x90; out[n++] = 0;
    *out_length = n;
    s->state = 1;
    return 1;
fail:
    sl_session_reset(s);
    return 0;
}

static size_t make_salt(const sl_session *s, int persistent, uint32_t passcode,
                        const uint8_t public_key[65], uint8_t salt[160])
{
    size_t n = 32;
    memcpy(salt, initiator_longterm_x, 32);
    const char *label = persistent ? "Persistent" : "Volatile";
    size_t label_length = strlen(label);
    memcpy(salt + n, label, label_length); n += label_length;
    memcpy(salt + n, s->initiator_id, 16); n += 16;
    salt[n++] = 0xc3; salt[n++] = 0x5c; salt[n++] = 2;
    memcpy(salt + n, s->version, 2); n += 2;
    memcpy(salt + n, s->initiator_public + 1, 32); n += 32;
    memcpy(salt + n, s->transaction, 16); n += 16;
    salt[n++] = persistent ? s->flags : (s->flags & 1);
    /* Python's volatile salt deliberately omits the passcode despite its
     * docstring. Preserve actual wire behavior; persistent includes it. */
    if (persistent) {
        for (int i = 0; i < 4; ++i) salt[n++] = (uint8_t)(passcode >> (8 * i));
        memcpy(salt + n, public_key + 1, 32); n += 32;
    }
    return n;
}

int sl_auth1(sl_session *s, const uint8_t *payload, size_t length,
             const uint8_t private_key[32], const uint8_t responder_id[16], uint32_t passcode,
             uint8_t *out, size_t capacity, size_t *out_length)
{
    uint8_t secret[32] = {0}, kdh[32] = {0}, keys[64] = {0}, salt[160] = {0};
    uint8_t public_key[65], info[48], transcript[104], signature[64], plain[133], nonce[12];
    size_t offset = 0, n = 0, salt_length;
    sl_tlv entry;
    int status, found = 0, ok = 0;
    *out_length = 0;
    if (s->state != 1 || !passcode || capacity < 155) goto done;
    while ((status = sl_tlv_next(payload, length, &offset, &entry)) > 0) {
        if (entry.tag == 0x9e) {
            if (found || entry.length != 64) goto done;
            found = 1;
        }
    }
    /* Existing protocol does not verify the initiator signature: the configured
     * constant public key is used for KDF salt only. Do not invent a new trust
     * policy during migration; pairing/authorization is a separate review. */
    if (status < 0 || !found || !sl_public_key(private_key, public_key) ||
        !sl_ecdh(s->ephemeral_private, s->initiator_public, secret) ||
        !sl_kdh(secret, s->transaction, 1, kdh)) goto done;
    memcpy(info, s->ephemeral_public + 1, 32);
    memcpy(info + 32, responder_id, 16);
    salt_length = make_salt(s, 0, passcode, public_key, salt);
    if (!sl_hkdf(kdh, 32, salt, salt_length, info, 48, keys, 64)) goto done;
    salt_length = make_salt(s, 1, passcode, public_key, salt);
    if (!sl_hkdf(kdh, 32, salt, salt_length, info, 48, s->pending_persistent, 32)) goto done;
    if (!sl_tlv_put(transcript, sizeof(transcript), &n, 0x4d, s->initiator_id, 16) ||
        !sl_tlv_put(transcript, sizeof(transcript), &n, 0x86, s->ephemeral_public + 1, 32) ||
        !sl_tlv_put(transcript, sizeof(transcript), &n, 0x87, s->initiator_public + 1, 32) ||
        !sl_tlv_put(transcript, sizeof(transcript), &n, 0x4c, s->transaction, 16) ||
        !sl_sign(private_key, transcript, n, signature)) goto done;
    n = 0;
    if (!sl_tlv_put(plain, sizeof(plain), &n, 0x5a, public_key, 65) ||
        !sl_tlv_put(plain, sizeof(plain), &n, 0x9e, signature, 64)) goto done;
    nonce_for(1, nonce);
    if (!sl_encrypt(keys + 32, 32, nonce, plain, n, out + 4, out + 4 + n)) goto done;
    out[0] = 0; out[1] = 1; out[2] = 0; out[3] = (uint8_t)(n + 16);
    out[n + 20] = 0x90; out[n + 21] = 0;
    *out_length = n + 22;
    memcpy(s->incoming_key, keys, 32);
    memcpy(s->outgoing_key, keys + 32, 32);
    s->incoming_counter = s->outgoing_counter = 2;
    s->state = 2;
    OPENSSL_cleanse(s->ephemeral_private, 32);
    ok = 1;
done:
    OPENSSL_cleanse(secret, sizeof(secret));
    OPENSSL_cleanse(kdh, sizeof(kdh));
    OPENSSL_cleanse(keys, sizeof(keys));
    OPENSSL_cleanse(salt, sizeof(salt));
    if (!ok) sl_session_reset(s);
    return ok;
}

int sl_session_decrypt(sl_session *s, const uint8_t *payload, size_t length,
                       uint8_t *out, size_t capacity, size_t *out_length)
{
    uint8_t nonce[12];
    *out_length = 0;
    if (s->state != 2 || length < 16 || length - 16 > capacity ||
        s->incoming_counter > 65535) return 0;
    nonce_for(s->incoming_counter, nonce);
    if (!sl_decrypt(s->incoming_key, 32, nonce, payload, length - 16,
                    payload + length - 16, out)) return 0;
    ++s->incoming_counter;
    *out_length = length - 16;
    return 1;
}

int sl_session_encrypt(sl_session *s, const uint8_t *payload, size_t length,
                       uint8_t *out, size_t capacity, size_t *out_length)
{
    uint8_t nonce[12];
    *out_length = 0;
    if (s->state != 2 || capacity < 16 || length > capacity - 16 ||
        s->outgoing_counter > 65535) return 0;
    nonce_for(s->outgoing_counter, nonce);
    if (!sl_encrypt(s->outgoing_key, 32, nonce, payload, length, out, out + length)) return 0;
    ++s->outgoing_counter;
    *out_length = length + 16;
    return 1;
}
