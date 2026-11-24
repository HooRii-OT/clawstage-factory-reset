#include "shadowlink.h"
#include <limits.h>
#include <string.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/param_build.h>

/* Use provider-based EVP operations (OpenSSL 3), without deprecated EC_KEY. */
int sl_public_key(const uint8_t private_key[32], uint8_t public_key[65])
{
    int ok = 0;
    EC_GROUP *group = EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1);
    EC_POINT *point = group ? EC_POINT_new(group) : NULL;
    BIGNUM *scalar = BN_bin2bn(private_key, 32, NULL);
    BN_CTX *ctx = BN_CTX_new();
    if (!group || !point || !scalar || !ctx || BN_is_zero(scalar) ||
        BN_cmp(scalar, EC_GROUP_get0_order(group)) >= 0) goto done;
    BN_set_flags(scalar, BN_FLG_CONSTTIME);
    ok = EC_POINT_mul(group, point, scalar, NULL, NULL, ctx) == 1 &&
         EC_POINT_point2oct(group, point, POINT_CONVERSION_UNCOMPRESSED,
                            public_key, 65, ctx) == 65;
done:
    BN_clear_free(scalar);
    EC_POINT_free(point);
    EC_GROUP_free(group);
    BN_CTX_free(ctx);
    return ok;
}

static EVP_PKEY *import_key(const uint8_t *private_key, const uint8_t public_key[65])
{
    EVP_PKEY *key = NULL;
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
    OSSL_PARAM_BLD *builder = OSSL_PARAM_BLD_new();
    OSSL_PARAM *params = NULL;
    BIGNUM *scalar = private_key ? BN_bin2bn(private_key, 32, NULL) : NULL;
    if (!ctx || !builder || public_key[0] != 4 || (private_key && !scalar)) goto done;
    if (!OSSL_PARAM_BLD_push_utf8_string(builder, OSSL_PKEY_PARAM_GROUP_NAME, "prime256v1", 0) ||
        !OSSL_PARAM_BLD_push_octet_string(builder, OSSL_PKEY_PARAM_PUB_KEY, public_key, 65) ||
        (scalar && !OSSL_PARAM_BLD_push_BN(builder, OSSL_PKEY_PARAM_PRIV_KEY, scalar))) goto done;
    params = OSSL_PARAM_BLD_to_param(builder);
    if (!params || EVP_PKEY_fromdata_init(ctx) <= 0 ||
        EVP_PKEY_fromdata(ctx, &key, private_key ? EVP_PKEY_KEYPAIR : EVP_PKEY_PUBLIC_KEY, params) <= 0) {
        EVP_PKEY_free(key);
        key = NULL;
    }
done:
    OSSL_PARAM_free(params);
    OSSL_PARAM_BLD_free(builder);
    BN_clear_free(scalar);
    EVP_PKEY_CTX_free(ctx);
    return key;
}

static EVP_PKEY *private_pkey(const uint8_t private_key[32])
{
    uint8_t public_key[65];
    return sl_public_key(private_key, public_key) ? import_key(private_key, public_key) : NULL;
}

int sl_keygen(uint8_t private_key[32], uint8_t public_key[65])
{
    int ok = 0;
    BIGNUM *scalar = NULL;
    EVP_PKEY *key = EVP_EC_gen("prime256v1");
    if (key && EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_PRIV_KEY, &scalar) == 1 &&
        BN_bn2binpad(scalar, private_key, 32) == 32 &&
        sl_public_key(private_key, public_key)) ok = 1;
    if (!ok) OPENSSL_cleanse(private_key, 32);
    BN_clear_free(scalar);
    EVP_PKEY_free(key);
    return ok;
}

int sl_ecdh(const uint8_t private_key[32], const uint8_t peer[65], uint8_t secret[32])
{
    size_t length = 32;
    EVP_PKEY *key = private_pkey(private_key), *remote = import_key(NULL, peer);
    EVP_PKEY_CTX *ctx = key ? EVP_PKEY_CTX_new(key, NULL) : NULL;
    int ok = ctx && remote && EVP_PKEY_derive_init(ctx) > 0 &&
             EVP_PKEY_derive_set_peer(ctx, remote) > 0 &&
             EVP_PKEY_derive(ctx, secret, &length) > 0 && length == 32;
    if (!ok) OPENSSL_cleanse(secret, 32);
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(remote);
    EVP_PKEY_free(key);
    return ok;
}

/* Compatibility: Python signs SHA256(transcript) with ECDSA(SHA256),
 * therefore the signature covers a double SHA256, not a prehashed digest. */
int sl_sign(const uint8_t private_key[32], const uint8_t *data, size_t length,
            uint8_t signature[64])
{
    uint8_t digest[32], der[80];
    size_t n = sizeof(der);
    const unsigned char *cursor = der;
    const BIGNUM *r, *s;
    ECDSA_SIG *sig = NULL;
    EVP_PKEY *key = private_pkey(private_key);
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    int ok = 0;
    if (!key || !ctx || !EVP_Digest(data, length, digest, NULL, EVP_sha256(), NULL) ||
        EVP_DigestSignInit(ctx, NULL, EVP_sha256(), NULL, key) <= 0 ||
        EVP_DigestSign(ctx, der, &n, digest, sizeof(digest)) <= 0) goto done;
    sig = d2i_ECDSA_SIG(NULL, &cursor, (long)n);
    if (!sig) goto done;
    ECDSA_SIG_get0(sig, &r, &s);
    ok = BN_bn2binpad(r, signature, 32) == 32 && BN_bn2binpad(s, signature + 32, 32) == 32;
done:
    ECDSA_SIG_free(sig);
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    return ok;
}

int sl_verify(const uint8_t public_key[65], const uint8_t *data, size_t length,
              const uint8_t signature[64])
{
    uint8_t digest[32], der[80];
    unsigned char *cursor = der;
    BIGNUM *r = BN_bin2bn(signature, 32, NULL), *s = BN_bin2bn(signature + 32, 32, NULL);
    ECDSA_SIG *sig = ECDSA_SIG_new();
    EVP_PKEY *key = import_key(NULL, public_key);
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    int ok = 0, n;
    if (!r || !s || !sig || !key || !ctx || !ECDSA_SIG_set0(sig, r, s)) goto done;
    r = s = NULL;
    n = i2d_ECDSA_SIG(sig, &cursor);
    if (n <= 0) goto done;
    ok = EVP_Digest(data, length, digest, NULL, EVP_sha256(), NULL) &&
         EVP_DigestVerifyInit(ctx, NULL, EVP_sha256(), NULL, key) > 0 &&
         EVP_DigestVerify(ctx, der, (size_t)n, digest, sizeof(digest)) == 1;
done:
    BN_free(r);
    BN_free(s);
    ECDSA_SIG_free(sig);
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    return ok;
}

int sl_kdh(const uint8_t secret[32], const uint8_t transaction[16],
           uint32_t counter, uint8_t out[32])
{
    uint8_t data[52];
    memcpy(data, secret, 32);
    for (int i = 0; i < 4; ++i) data[32 + i] = (uint8_t)(counter >> (24 - 8 * i));
    memcpy(data + 36, transaction, 16);
    int ok = EVP_Digest(data, sizeof(data), out, NULL, EVP_sha256(), NULL);
    OPENSSL_cleanse(data, sizeof(data));
    return ok;
}

int sl_hkdf(const uint8_t *key, size_t key_length, const uint8_t *salt,
            size_t salt_length, const uint8_t *info, size_t info_length,
            uint8_t *out, size_t out_length)
{
    EVP_KDF *kdf = EVP_KDF_fetch(NULL, "HKDF", NULL);
    EVP_KDF_CTX *ctx = kdf ? EVP_KDF_CTX_new(kdf) : NULL;
    OSSL_PARAM params[] = {
        OSSL_PARAM_utf8_string(OSSL_KDF_PARAM_DIGEST, "SHA256", 0),
        OSSL_PARAM_octet_string(OSSL_KDF_PARAM_KEY, (void *)key, key_length),
        OSSL_PARAM_octet_string(OSSL_KDF_PARAM_SALT, (void *)salt, salt_length),
        OSSL_PARAM_octet_string(OSSL_KDF_PARAM_INFO, (void *)info, info_length),
        OSSL_PARAM_END
    };
    int ok = ctx && EVP_KDF_derive(ctx, out, out_length, params) > 0;
    EVP_KDF_CTX_free(ctx);
    EVP_KDF_free(kdf);
    return ok;
}

static const EVP_CIPHER *cipher_for(size_t key_length)
{
    return key_length == 16 ? EVP_aes_128_gcm() : key_length == 32 ? EVP_aes_256_gcm() : NULL;
}

int sl_encrypt(const uint8_t *key, size_t key_length, const uint8_t nonce[12],
               const uint8_t *in, size_t length, uint8_t *out, uint8_t tag[16])
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    const EVP_CIPHER *cipher = cipher_for(key_length);
    int n = 0, final = 0;
    int ok = ctx && cipher && length <= INT_MAX &&
        EVP_EncryptInit_ex(ctx, cipher, NULL, key, nonce) == 1 &&
        EVP_EncryptUpdate(ctx, out, &n, in, (int)length) == 1 &&
        EVP_EncryptFinal_ex(ctx, out + n, &final) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag) == 1;
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

int sl_decrypt(const uint8_t *key, size_t key_length, const uint8_t nonce[12],
               const uint8_t *in, size_t length, const uint8_t tag[16], uint8_t *out)
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    const EVP_CIPHER *cipher = cipher_for(key_length);
    int n = 0, final = 0;
    int ok = ctx && cipher && length <= INT_MAX &&
        EVP_DecryptInit_ex(ctx, cipher, NULL, key, nonce) == 1 &&
        EVP_DecryptUpdate(ctx, out, &n, in, (int)length) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, (void *)tag) == 1 &&
        EVP_DecryptFinal_ex(ctx, out + n, &final) == 1;
    if (!ok) OPENSSL_cleanse(out, length);
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}
