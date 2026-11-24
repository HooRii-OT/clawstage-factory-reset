#ifndef SHADOWLINK_H
#define SHADOWLINK_H

#include <stddef.h>
#include <stdint.h>

#define SL_MAX_FRAME 8192
typedef struct {
    uint16_t tag;
    const uint8_t *value;
    size_t length;
} sl_tlv;

/* 1 = entry, 0 = end, -1 = malformed. Input is borrowed. */
int sl_tlv_next(const uint8_t *data, size_t length, size_t *offset, sl_tlv *entry);
int sl_tlv_put(uint8_t *out, size_t capacity, size_t *length,
               uint16_t tag, const uint8_t *value, size_t value_length);
typedef int (*sl_frame_callback)(const uint8_t *, size_t, void *);
typedef struct {
    uint8_t data[SL_MAX_FRAME];
    size_t length;
    uint64_t last_ms;
} sl_receiver;
/* Monotonic milliseconds; offset=0 means append (mobile fragmentation). */
int sl_receive(sl_receiver *rx, const uint8_t *data, size_t length,
               size_t offset, uint64_t now_ms, sl_frame_callback callback, void *user);

/* Crypto functions return 1 on success, 0 on failure. P-256 keys use
 * 32-byte private scalars and 65-byte SEC1 uncompressed public points. */
int sl_keygen(uint8_t private_key[32], uint8_t public_key[65]);
int sl_public_key(const uint8_t private_key[32], uint8_t public_key[65]);
int sl_ecdh(const uint8_t private_key[32], const uint8_t peer[65], uint8_t secret[32]);
int sl_sign(const uint8_t private_key[32], const uint8_t *data, size_t length,
            uint8_t signature[64]);
int sl_verify(const uint8_t public_key[65], const uint8_t *data, size_t length,
              const uint8_t signature[64]);
int sl_kdh(const uint8_t secret[32], const uint8_t transaction[16],
           uint32_t counter, uint8_t out[32]);
int sl_hkdf(const uint8_t *key, size_t key_length, const uint8_t *salt,
            size_t salt_length, const uint8_t *info, size_t info_length,
            uint8_t *out, size_t out_length);
/* Output capacity must be length bytes; tag is separate. On authentication
 * failure decrypt wipes the output. In-place input/output is supported. */
int sl_encrypt(const uint8_t *key, size_t key_length, const uint8_t nonce[12],
               const uint8_t *in, size_t length, uint8_t *out, uint8_t tag[16]);
int sl_decrypt(const uint8_t *key, size_t key_length, const uint8_t nonce[12],
               const uint8_t *in, size_t length, const uint8_t tag[16], uint8_t *out);

typedef struct {
    uint8_t ephemeral_private[32], ephemeral_public[65], initiator_public[65];
    uint8_t transaction[16], initiator_id[16], version[2], flags;
    uint8_t incoming_key[32], outgoing_key[32], pending_persistent[32];
    uint32_t incoming_counter, outgoing_counter;
    int state; /* 0 = idle, 1 = AUTH0, 2 = AUTH1 */
} sl_session;
void sl_session_reset(sl_session *session);
/* Payload input; complete frame output (AUTH responses include 9000 outside
 * the payload length). Caller owns config persistence and peer isolation. */
int sl_auth0(sl_session *session, const uint8_t *payload, size_t length,
             const uint8_t responder_id[16], uint8_t *out, size_t capacity, size_t *out_length);
int sl_auth1(sl_session *session, const uint8_t *payload, size_t length,
             const uint8_t private_key[32], const uint8_t responder_id[16], uint32_t passcode,
             uint8_t *out, size_t capacity, size_t *out_length);
int sl_session_decrypt(sl_session *session, const uint8_t *payload, size_t length,
                       uint8_t *out, size_t capacity, size_t *out_length);
int sl_session_encrypt(sl_session *session, const uint8_t *payload, size_t length,
                       uint8_t *out, size_t capacity, size_t *out_length);
/* Encode the existing SL: onboarding string (no PNG rendering). */
int sl_onboarding(uint16_t vendor, uint16_t product, uint16_t discriminator,
                   uint32_t passcode, const uint8_t responder_id[16],
                   char *out, size_t capacity);
/* Ten-byte service-data payload; the BlueZ layer supplies its UUID. */
int sl_advertisement(uint16_t discriminator, int commissioned,
                     const uint8_t responder_id[16], const uint8_t public_key[65],
                     uint8_t out[10]);

#endif
