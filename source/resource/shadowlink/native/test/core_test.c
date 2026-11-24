#include "shadowlink.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static int count_frame(const uint8_t *data, size_t length, void *user)
{
    CHECK(length == 4 + (((size_t)data[2] << 8) | data[3]));
    ++*(int *)user;
    return 1;
}

int main(void)
{
    uint8_t value[512] = {0}, encoded[520];
    size_t lengths[] = {0, 1, 127, 128, 255, 256, 512};
    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
        size_t n = 0, offset = 0;
        sl_tlv entry;
        CHECK(sl_tlv_put(encoded, sizeof(encoded), &n, 0x7f66, value, lengths[i]));
        CHECK(sl_tlv_next(encoded, n, &offset, &entry) == 1);
        CHECK(entry.tag == 0x7f66 && entry.length == lengths[i] && offset == n);
        CHECK(sl_tlv_next(encoded, n, &offset, &entry) == 0);
        for (size_t cut = 1; cut < n; ++cut) {
            offset = 0;
            CHECK(sl_tlv_next(encoded, cut, &offset, &entry) == -1);
        }
    }
    {
        size_t offset = 0, n = sizeof(encoded);
        sl_tlv entry;
        const uint8_t unsupported_length[] = {0x10, 0x83, 0, 0, 0};
        CHECK(sl_tlv_next(unsupported_length, sizeof(unsupported_length), &offset, &entry) == -1);
        CHECK(!sl_tlv_put(encoded, sizeof(encoded), &n, 0x10, value, 1));
        n = 0;
        CHECK(!sl_tlv_put(encoded, sizeof(encoded), &n, 0x1234, value, 1));
        CHECK(!sl_tlv_put(encoded, sizeof(encoded), &n, 0x7f, value, 1));
    }
    sl_receiver rx = {0};
    uint8_t frames[] = {0, 0, 0, 2, 0x41, 0, 5, 10, 0, 0};
    for (size_t split = 0; split <= sizeof(frames); ++split) {
        int count = 0;
        memset(&rx, 0, sizeof(rx));
        CHECK(sl_receive(&rx, frames, split, 0, 1, count_frame, &count));
        CHECK(sl_receive(&rx, frames + split, sizeof(frames) - split, 0, 2, count_frame, &count));
        CHECK(count == 2 && rx.length == 0);
    }
    int count = 0;
    CHECK(sl_receive(&rx, frames, 3, 0, 1, count_frame, &count));
    CHECK(!sl_receive(&rx, frames, 1, 2, 2, count_frame, &count));
    CHECK(rx.length == 0);
    CHECK(sl_receive(&rx, frames, 3, 0, 3, count_frame, &count));
    CHECK(sl_receive(&rx, frames, sizeof(frames), 0, 10004, count_frame, &count));
    CHECK(count == 2);
    const uint8_t oversized[] = {0, 0, 0xff, 0xff};
    CHECK(!sl_receive(&rx, oversized, 4, 0, 10005, count_frame, &count));

    uint8_t sk[32], pk[65], peer_sk[32], peer_pk[65], a[32], b[32], sig[64];
    CHECK(sl_keygen(sk, pk) && sl_keygen(peer_sk, peer_pk));
    CHECK(sl_ecdh(sk, peer_pk, a) && sl_ecdh(peer_sk, pk, b));
    CHECK(memcmp(a, b, 32) == 0);
    CHECK(sl_sign(sk, frames, sizeof(frames), sig));
    CHECK(sl_verify(pk, frames, sizeof(frames), sig));
    sig[5] ^= 1;
    CHECK(!sl_verify(pk, frames, sizeof(frames), sig));
    memset(sk, 0, sizeof(sk));
    CHECK(!sl_public_key(sk, pk));
    memset(peer_pk, 0, sizeof(peer_pk));
    CHECK(!sl_ecdh(peer_sk, peer_pk, a));

    uint8_t key[32] = {0}, nonce[12] = {0}, tag[16], cipher[10], plain[10];
    for (size_t key_length = 16; key_length <= 32; key_length += 16) {
        CHECK(sl_encrypt(key, key_length, nonce, frames, sizeof(frames), cipher, tag));
        CHECK(sl_decrypt(key, key_length, nonce, cipher, sizeof(cipher), tag, plain));
        CHECK(memcmp(plain, frames, sizeof(frames)) == 0);
        tag[0] ^= 1;
        CHECK(!sl_decrypt(key, key_length, nonce, cipher, sizeof(cipher), tag, plain));
        for (size_t i = 0; i < sizeof(plain); ++i) CHECK(plain[i] == 0);
    }
    sl_session session = {0};
    uint8_t auth0[160], response[256], auth1[66] = {0x9e, 0x40};
    uint8_t device_key[32], device_public[65], identity[16] = {0}, transaction[16] = {1};
    size_t request_length = 0, response_length = 0;
    CHECK(sl_keygen(peer_sk, peer_pk) && sl_keygen(device_key, device_public));
    CHECK(sl_tlv_put(auth0, sizeof(auth0), &request_length, 0x87, peer_pk, 65));
    CHECK(sl_tlv_put(auth0, sizeof(auth0), &request_length, 0x4c, transaction, 16));
    CHECK(sl_tlv_put(auth0, sizeof(auth0), &request_length, 0x4d, identity, 16));
    CHECK(!sl_auth1(&session, auth1, sizeof(auth1), device_key, identity, 20202021,
                    response, sizeof(response), &response_length));
    CHECK(sl_auth0(&session, auth0, request_length, identity, response, sizeof(response), &response_length));
    CHECK(response_length == 91 && session.state == 1);
    CHECK(sl_auth1(&session, auth1, sizeof(auth1), device_key, identity, 20202021,
                   response, sizeof(response), &response_length));
    CHECK(response_length == 155 && session.state == 2);
    CHECK(session.incoming_counter == 2 && session.outgoing_counter == 2);
    memset(nonce, 0, sizeof(nonce)); nonce[7] = 1; nonce[11] = 2;
    CHECK(sl_encrypt(session.incoming_key, 32, nonce, frames, sizeof(frames), response, response + sizeof(frames)));
    CHECK(sl_session_decrypt(&session, response, sizeof(frames) + 16, encoded, sizeof(encoded), &response_length));
    CHECK(response_length == sizeof(frames) && memcmp(encoded, frames, sizeof(frames)) == 0);
    CHECK(!sl_session_decrypt(&session, response, sizeof(frames) + 16, encoded, sizeof(encoded), &response_length));
    CHECK(session.incoming_counter == 3 && session.outgoing_counter == 2);
    CHECK(sl_session_encrypt(&session, frames, sizeof(frames), response, sizeof(response), &response_length));
    CHECK(sl_decrypt(session.outgoing_key, 32, nonce, response, sizeof(frames), response + sizeof(frames), encoded));
    CHECK(memcmp(encoded, frames, sizeof(frames)) == 0);
    session.outgoing_counter = 65536;
    CHECK(!sl_session_encrypt(&session, frames, sizeof(frames), response, sizeof(response), &response_length));
    sl_session_reset(&session);
    const uint8_t *session_bytes = (const uint8_t *)&session;
    for (size_t i = 0; i < sizeof(session); ++i) CHECK(session_bytes[i] == 0);
    puts("Shadowlink native core tests passed");
    return 0;
}
