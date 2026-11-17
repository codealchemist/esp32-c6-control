#include "prov_crypto.h"

#include <mbedtls/base64.h>
#include <mbedtls/gcm.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>

#include <string.h>

namespace prov_crypto {

bool hkdfSha256(const uint8_t *ikm, size_t ikmLen, const uint8_t *salt, size_t saltLen,
                const char *info, uint8_t *out, size_t outLen) {
  const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (md == nullptr) return false;
  // strlen, not sizeof: the spec's info strings are the ASCII bytes only,
  // with no terminator. Including the NUL would derive different keys than
  // every other implementation of this spec.
  return mbedtls_hkdf(md, salt, saltLen, ikm, ikmLen, reinterpret_cast<const uint8_t *>(info),
                      strlen(info), out, outLen) == 0;
}

namespace {

// Both seal and open need the same setup/teardown; the only difference is
// which mbedTLS call sits in the middle.
struct GcmCtx {
  mbedtls_gcm_context ctx;
  bool ok = false;

  explicit GcmCtx(const uint8_t key[kKeyLen]) {
    mbedtls_gcm_init(&ctx);
    ok = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, kKeyLen * 8) == 0;
  }
  ~GcmCtx() { mbedtls_gcm_free(&ctx); }
};

}  // namespace

bool gcmSeal(const uint8_t key[kKeyLen], const uint8_t nonce[kNonceLen], const uint8_t *aad,
             size_t aadLen, const uint8_t *pt, size_t ptLen, uint8_t *ctOut,
             uint8_t tagOut[kTagLen]) {
  GcmCtx g(key);
  if (!g.ok) return false;
  return mbedtls_gcm_crypt_and_tag(&g.ctx, MBEDTLS_GCM_ENCRYPT, ptLen, nonce, kNonceLen, aad,
                                   aadLen, pt, ctOut, kTagLen, tagOut) == 0;
}

bool gcmOpen(const uint8_t key[kKeyLen], const uint8_t nonce[kNonceLen], const uint8_t *aad,
             size_t aadLen, const uint8_t *ct, size_t ctLen, const uint8_t tag[kTagLen],
             uint8_t *ptOut) {
  GcmCtx g(key);
  if (!g.ok) return false;
  // Returns MBEDTLS_ERR_GCM_AUTH_FAILED on a bad tag — treated identically to
  // any other error by the caller, which must not distinguish them.
  return mbedtls_gcm_auth_decrypt(&g.ctx, ctLen, nonce, kNonceLen, aad, aadLen, tag, kTagLen, ct,
                                  ptOut) == 0;
}

String base64UrlEncode(const uint8_t *data, size_t len) {
  // mbedTLS emits standard base64 with padding; base64url is the same
  // alphabet with two substitutions and the padding removed (§8).
  size_t needed = 0;
  mbedtls_base64_encode(nullptr, 0, &needed, data, len);
  if (needed == 0) return String();

  uint8_t *buf = static_cast<uint8_t *>(malloc(needed));
  if (buf == nullptr) return String();

  size_t written = 0;
  if (mbedtls_base64_encode(buf, needed, &written, data, len) != 0) {
    free(buf);
    return String();
  }

  String out;
  out.reserve(written);
  for (size_t i = 0; i < written; i++) {
    const char c = static_cast<char>(buf[i]);
    if (c == '=') break;  // unpadded
    out += (c == '+') ? '-' : (c == '/') ? '_' : c;
  }
  free(buf);
  return out;
}

}  // namespace prov_crypto
