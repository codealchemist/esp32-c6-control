#pragma once

#include <Arduino.h>

// Crypto primitives for Provisioning Protocol v2
// (docs/PROVISIONING_PROTOCOL_V2.md §4-§5). Thin wrappers over mbedTLS,
// which is already linked by the ESP-IDF build — no new dependency.
//
// Deliberately narrow: this exposes exactly the four operations the protocol
// needs, with the algorithm choices baked in rather than parameterised. There
// is no cipher negotiation in the protocol, so there is nothing to configure
// and no way for a caller to pick something weaker by accident.

namespace prov_crypto {

constexpr size_t kKeyLen = 32;    // AES-256
constexpr size_t kNonceLen = 12;  // GCM standard
constexpr size_t kTagLen = 16;
constexpr size_t kTokenLen = 32;  // session token, §8

// HKDF-SHA256. `info` is a NUL-terminated ASCII string; the NUL is NOT
// included in the derivation (§4: "no trailing NUL").
bool hkdfSha256(const uint8_t *ikm, size_t ikmLen, const uint8_t *salt, size_t saltLen,
                const char *info, uint8_t *out, size_t outLen);

// AES-256-GCM encrypt. `tagOut` is separate from `ctOut`; the wire format
// appends it (§5). `ctOut` must have room for `ptLen` bytes.
bool gcmSeal(const uint8_t key[kKeyLen], const uint8_t nonce[kNonceLen], const uint8_t *aad,
             size_t aadLen, const uint8_t *pt, size_t ptLen, uint8_t *ctOut,
             uint8_t tagOut[kTagLen]);

// AES-256-GCM decrypt + verify. Returns false on ANY authentication failure;
// on false, `ptOut` contents are undefined and must not be used. mbedTLS does
// the tag comparison in constant time.
bool gcmOpen(const uint8_t key[kKeyLen], const uint8_t nonce[kNonceLen], const uint8_t *aad,
             size_t aadLen, const uint8_t *ct, size_t ctLen, const uint8_t tag[kTagLen],
             uint8_t *ptOut);

// base64url, unpadded — the QR token encoding (§8).
String base64UrlEncode(const uint8_t *data, size_t len);

}  // namespace prov_crypto
