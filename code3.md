# JWT Implementation Assessment — `jwt.c`

## Overall Verdict

The implementation is **fundamentally correct** and follows the JWT/JWS (RFC 7519 / RFC 7515) structure properly. It uses strong cryptographic primitives from libsodium, handles key material hygienically, and the general flow (create → base64url-encode → HMAC-sign → verify) is sound. However, there is **one significant security bug** in signature verification and several areas that could be hardened.

---

## ✅ What's Done Well

### 1. Correct JWT Structure
The token follows the standard three-part format: `header.payload.signature`, using base64url-no-padding encoding (per RFC 7515 §2), with the header `{"alg":"HS256","typ":"JWT"}`.

### 2. Strong Cryptographic Primitives
- Uses `crypto_auth_hmacsha256` from libsodium — a well-audited, constant-time HMAC implementation.
- Uses `sodium_base64_VARIANT_URLSAFE_NO_PADDING` — exactly what the JWT spec requires.
- Uses `randombytes_buf()` for UUIDv4 generation — cryptographically secure.

### 3. Key Material Hygiene
Secret key bytes are consistently wiped via `sodium_memzero()` after use in:
- [`jwt_create()`](file:///home/ubuntu/evhttp/src/jwt.c#L137)
- [`jwt_verify_mac()`](file:///home/ubuntu/evhttp/src/jwt.c#L160)
- [`jwt_get_expiration()`](file:///home/ubuntu/evhttp/src/jwt.c#L72) (zeroes payload buffer)
- [`jwt_verify()`](file:///home/ubuntu/evhttp/src/jwt.c#L226) (zeroes payload buffer)

### 4. Algorithm Validation on Verify
[`jwt_check_header_alg()`](file:///home/ubuntu/evhttp/src/jwt.c#L141-L152) correctly validates that the header's `alg` field is `"HS256"` before proceeding. This prevents "algorithm confusion" attacks (e.g., an attacker setting `alg: none` to bypass verification).

### 5. Correct Verification Order
In [`jwt_verify()`](file:///home/ubuntu/evhttp/src/jwt.c#L209-L228), signature is verified **before** the payload is parsed and trusted — this is the correct order to prevent processing untrusted data.

### 6. Input Validation & Defensive Coding
- Null checks on all public API entry points.
- Buffer overflow protection via size checks before base64 encoding.
- JSON string escaping of the username via `json_encode_string()` to prevent JSON injection.
- `snprintf` used throughout with length checks.

---

## 🔴 Critical Issue

### Timing-Unsafe Signature Comparison in `jwt_verify_mac()`

**Location:** [`jwt_verify_mac()` line 171](file:///home/ubuntu/evhttp/src/jwt.c#L171)

```c
int match = sodium_memcmp(provided_sig, mac_b64, sizeof(mac_b64));
```

> [!CAUTION]
> `sodium_memcmp` compares exactly `sizeof(mac_b64)` = **128 bytes**, but the actual base64-encoded signature is only ~43 characters. The remaining bytes in the 128-byte buffers are initialized to `{0}`. This means the comparison is comparing the correct signature bytes **plus ~85 trailing zero bytes**.

**Why this is still technically safe (but fragile):**

Both `mac_b64` and `provided_sig` are zero-initialized (`= {0}`), and the comparison length covers the full 128-byte buffer. Since both buffers have zeros after the signature content, matching zeros pad the comparison. If signatures match, the trailing zeros match too — so it works. **However:**

1. **If `provided_sig` contains a string shorter than `mac_b64`** — the trailing zeros will still match, but `snprintf` will null-terminate, so the bytes beyond are already zero. This is fine.
2. **If `provided_sig` is longer than 127 bytes** — `snprintf` truncates it, meaning a long signature could falsely pass if the first 127 bytes match. This is an unlikely but real edge case.

**The real problem:** `provided_sig` is populated via:
```c
(void)snprintf(provided_sig, sizeof(provided_sig), "%s", signature);
```
If the attacker provides a signature longer than 127 chars, it gets truncated. `sodium_memcmp` then compares the truncated version against the expected MAC. This would not cause a false positive in practice (the expected MAC is ~43 chars, so byte 44+ would differ), but it's **unnecessarily complex and fragile**.

**Recommended fix:** Compare only the meaningful bytes:
```c
size_t sig_len = strlen(mac_b64);
if (strlen(signature) != sig_len) return false;
return sodium_memcmp(signature, mac_b64, sig_len) == 0;
```

Or better yet, decode the provided signature to raw bytes and use `sodium_memcmp` on the raw HMAC bytes:
```c
unsigned char provided_mac[crypto_auth_hmacsha256_BYTES];
size_t provided_len = 0;
if (sodium_base642bin(provided_mac, sizeof(provided_mac), signature, strlen(signature),
                      NULL, &provided_len, NULL,
                      sodium_base64_VARIANT_URLSAFE_NO_PADDING) != 0)
    return false;
if (provided_len != crypto_auth_hmacsha256_BYTES)
    return false;
return sodium_memcmp(mac, provided_mac, sizeof(mac)) == 0;
```
This avoids all base64 string comparison issues entirely.

---

## 🟡 Moderate Issues

### 1. Secret Key Must Be Exactly 32 Hex Bytes (Rigid)

**Location:** [`get_secret_bytes()` line 81](file:///home/ubuntu/evhttp/src/jwt.c#L81)

```c
return secret_bin_len == crypto_auth_hmacsha256_KEYBYTES;  // 32 bytes
```

The function requires the hex secret to decode to **exactly** 32 bytes (64 hex chars). This is correct for HMAC-SHA256's key size, but:
- There's no error message explaining *why* it failed.
- HMAC-SHA256 per RFC 2104 accepts keys of any length (keys > block size are hashed, keys < block size are zero-padded). Enforcing exactly 32 bytes is stricter than the standard but is a reasonable security policy.

### 2. Fixed Buffer Sizes with No Overflow Detection

Throughout the code, fixed-size stack buffers are used:
- `payload[512]` in [`jwt_build_payload()`](file:///home/ubuntu/evhttp/src/jwt.c#L84)
- `msg[768]` in [`jwt_create()`](file:///home/ubuntu/evhttp/src/jwt.c#L130)
- `payload_json[8192]` in [`jwt_verify()`](file:///home/ubuntu/evhttp/src/jwt.c#L222) and [`jwt_get_expiration()`](file:///home/ubuntu/evhttp/src/jwt.c#L61)

These are generally adequate, but a maliciously crafted token with a very long base64 segment could cause `b64_decode_segment` to fail silently if the decoded payload exceeds 8191 bytes. The code does handle this correctly (returns `false`), but it's worth documenting the maximum token size the system supports.

### 3. No `iat` (Issued At) or `nbf` (Not Before) Claims

The payload contains `username`, `sessionId`, and `exp`, but omits:
- **`iat`** (issued at) — useful for audit trails and detecting clock skew.
- **`nbf`** (not before) — prevents tokens from being used before they're intended to be valid.
- **`iss`** (issuer) — standard claim to identify the issuing service.

These are optional per RFC 7519 but are standard best practice.

### 4. No Check for Extra Dots in Token

**Location:** [`jwt_verify()` line 209](file:///home/ubuntu/evhttp/src/jwt.c#L209-L215)

```c
const char* dot1 = strchr(token, '.');
const char* dot2 = strchr(dot1 + 1, '.');
```

The code finds the first two dots but doesn't verify there isn't a **third** dot. A token like `header.payload.sig.extra` would be accepted as valid (the `extra` part is silently ignored). Per RFC 7515, a JWS Compact Serialization has exactly two dots. This should be validated:

```c
if (strchr(dot2 + 1, '.') != NULL) return JWT_ERR_INVALID;
```

---

## 🟢 Minor / Stylistic Notes

### 1. `time()` Could Fail
[`jwt_build_payload()` line 88](file:///home/ubuntu/evhttp/src/jwt.c#L88) and [`jwt_parse_payload()` line 188](file:///home/ubuntu/evhttp/src/jwt.c#L188) call `time(nullptr)`, which can return `(time_t)(-1)` on error. This is extremely unlikely in practice but unhandled.

### 2. `%lld` Format Specifier
[`jwt_build_payload()` line 87](file:///home/ubuntu/evhttp/src/jwt.c#L87) uses `%lld` for `(long long)(time(nullptr) + timeout_seconds)`. This is correct and portable, but `time_t` is not guaranteed to be an integer type on all platforms (it could be a floating-point type on exotic systems). In practice, this is fine on all Linux/POSIX systems.

### 3. UUIDv4 Generation is Correct
[`generate_uuidv4()`](file:///home/ubuntu/evhttp/src/jwt.c#L10-L30) correctly sets version bits (byte 6, nibble = 0x4) and variant bits (byte 8, high bits = 0b10), per RFC 4122 §4.4.

---

## Summary Table

| Area | Status | Notes |
|---|---|---|
| JWT structure (header.payload.sig) | ✅ Correct | RFC 7519 compliant |
| Base64url encoding | ✅ Correct | Uses `URLSAFE_NO_PADDING` |
| HMAC-SHA256 signing | ✅ Correct | Uses libsodium |
| Algorithm header check | ✅ Correct | Prevents `alg: none` attacks |
| Signature verification (constant-time) | ⚠️ Fragile | Works but compares padded buffers; should compare raw bytes |
| Extra dots in token | 🟡 Missing | Accepts `a.b.c.d` as valid |
| Key hygiene (`sodium_memzero`) | ✅ Excellent | Secret bytes always wiped |
| Payload sanitization | ✅ Good | JSON-escapes username |
| Standard claims (`iat`, `nbf`, `iss`) | 🟡 Missing | Only `exp`, `username`, `sessionId` |
| Expiration check | ✅ Correct | Checked after signature verification |
| Buffer overflow protection | ✅ Good | Size checks before all base64 ops |
| Input validation | ✅ Good | Null checks on public APIs |

---

## Recommended Priority Fixes

1. **🔴 High**: Refactor `jwt_verify_mac()` to compare raw HMAC bytes instead of base64 strings — eliminates the fragile padded-buffer comparison.
2. **🟡 Medium**: Add a check for extra dots in the token during verification.
3. **🟡 Low**: Consider adding `iat` and `iss` claims to the payload for better auditability.
