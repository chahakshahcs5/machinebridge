#include "machinebridge/crypto.hpp"
#include "machinebridge/logger.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace machinebridge {

// ============================================================================
// 1. Base64 and Base64URL
// ============================================================================

namespace {

static const char B64_CHARS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

} // namespace

std::string base64_encode(const uint8_t* data, size_t length) {
    std::string out;
    out.reserve(((length + 2) / 3) * 4);

    size_t i = 0;
    while (i < length) {
        size_t rem = length - i;
        uint32_t octet_a = data[i++];
        uint32_t octet_b = (rem > 1) ? data[i++] : 0;
        uint32_t octet_c = (rem > 2) ? data[i++] : 0;

        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        out.push_back(B64_CHARS[(triple >> 18) & 0x3F]);
        out.push_back(B64_CHARS[(triple >> 12) & 0x3F]);
        out.push_back((rem > 1) ? B64_CHARS[(triple >> 6) & 0x3F] : '=');
        out.push_back((rem > 2) ? B64_CHARS[triple & 0x3F] : '=');
    }
    return out;
}

std::string base64url_encode(const uint8_t* data, size_t length) {
    std::string b64 = base64_encode(data, length);
    std::string out;
    out.reserve(b64.size());
    for (char c : b64) {
        if (c == '+') out.push_back('-');
        else if (c == '/') out.push_back('_');
        else if (c == '=') continue; // strip padding
        else out.push_back(c);
    }
    return out;
}

std::vector<uint8_t> base64_decode(std::string_view text) {
    std::vector<uint8_t> out;
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; ++i) T[static_cast<uint8_t>(B64_CHARS[i])] = i;

    int val = 0, valb = -8;
    for (char c : text) {
        if (c == '=') break;
        int v = T[static_cast<uint8_t>(c)];
        if (v == -1) continue;
        val = (val << 6) | v;
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

std::vector<uint8_t> base64url_decode(std::string_view text) {
    std::string standard(text);
    for (char& c : standard) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    while (standard.size() % 4 != 0) {
        standard.push_back('=');
    }
    return base64_decode(standard);
}

// ============================================================================
// 2. SHA-256 Implementation
// ============================================================================

namespace {

inline uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }
inline uint32_t choose(uint32_t e, uint32_t f, uint32_t g) { return (e & f) ^ (~e & g); }
inline uint32_t majority(uint32_t a, uint32_t b, uint32_t c) { return (a & b) ^ (a & c) ^ (b & c); }
inline uint32_t sig0(uint32_t x) { return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22); }
inline uint32_t sig1(uint32_t x) { return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25); }
inline uint32_t theta0(uint32_t x) { return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3); }
inline uint32_t theta1(uint32_t x) { return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10); }

static const uint32_t SHA256_K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

struct SHA256Context {
    uint32_t state[8];
    uint64_t bit_count;
    uint8_t buffer[64];
};

void sha256_init(SHA256Context& ctx) {
    ctx.state[0] = 0x6a09e667;
    ctx.state[1] = 0xbb67ae85;
    ctx.state[2] = 0x3c6ef372;
    ctx.state[3] = 0xa54ff53a;
    ctx.state[4] = 0x510e527f;
    ctx.state[5] = 0x9b05688c;
    ctx.state[6] = 0x1f83d9ab;
    ctx.state[7] = 0x5be0cd19;
    ctx.bit_count = 0;
}

void sha256_transform(SHA256Context& ctx, const uint8_t* data) {
    uint32_t W[64];
    for (int i = 0; i < 16; ++i) {
        W[i] = (static_cast<uint32_t>(data[i * 4]) << 24) |
               (static_cast<uint32_t>(data[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(data[i * 4 + 2]) << 8) |
               (static_cast<uint32_t>(data[i * 4 + 3]));
    }
    for (int i = 16; i < 64; ++i) {
        W[i] = theta1(W[i - 2]) + W[i - 7] + theta0(W[i - 15]) + W[i - 16];
    }

    uint32_t a = ctx.state[0], b = ctx.state[1], c = ctx.state[2], d = ctx.state[3];
    uint32_t e = ctx.state[4], f = ctx.state[5], g = ctx.state[6], h = ctx.state[7];

    for (int i = 0; i < 64; ++i) {
        uint32_t T1 = h + sig1(e) + choose(e, f, g) + SHA256_K[i] + W[i];
        uint32_t T2 = sig0(a) + majority(a, b, c);
        h = g; g = f; f = e; e = d + T1;
        d = c; c = b; b = a; a = T1 + T2;
    }

    ctx.state[0] += a; ctx.state[1] += b; ctx.state[2] += c; ctx.state[3] += d;
    ctx.state[4] += e; ctx.state[5] += f; ctx.state[6] += g; ctx.state[7] += h;
}

void sha256_update(SHA256Context& ctx, const uint8_t* data, size_t length) {
    size_t buffer_bytes = (ctx.bit_count / 8) % 64;
    ctx.bit_count += static_cast<uint64_t>(length) * 8;

    size_t offset = 0;
    if (buffer_bytes > 0) {
        size_t needed = 64 - buffer_bytes;
        size_t to_copy = (length < needed) ? length : needed;
        std::memcpy(ctx.buffer + buffer_bytes, data, to_copy);
        buffer_bytes += to_copy;
        offset += to_copy;
        if (buffer_bytes == 64) {
            sha256_transform(ctx, ctx.buffer);
            buffer_bytes = 0;
        }
    }

    while (offset + 64 <= length) {
        sha256_transform(ctx, data + offset);
        offset += 64;
    }

    if (offset < length) {
        std::memcpy(ctx.buffer + buffer_bytes, data + offset, length - offset);
    }
}

void sha256_final(SHA256Context& ctx, uint8_t hash[32]) {
    size_t buffer_bytes = (ctx.bit_count / 8) % 64;
    ctx.buffer[buffer_bytes++] = 0x80;

    if (buffer_bytes > 56) {
        std::memset(ctx.buffer + buffer_bytes, 0, 64 - buffer_bytes);
        sha256_transform(ctx, ctx.buffer);
        buffer_bytes = 0;
    }

    std::memset(ctx.buffer + buffer_bytes, 0, 56 - buffer_bytes);
    for (int i = 0; i < 8; ++i) {
        ctx.buffer[56 + i] = static_cast<uint8_t>((ctx.bit_count >> (56 - i * 8)) & 0xff);
    }
    sha256_transform(ctx, ctx.buffer);

    for (int i = 0; i < 8; ++i) {
        hash[i * 4]     = static_cast<uint8_t>((ctx.state[i] >> 24) & 0xff);
        hash[i * 4 + 1] = static_cast<uint8_t>((ctx.state[i] >> 16) & 0xff);
        hash[i * 4 + 2] = static_cast<uint8_t>((ctx.state[i] >> 8) & 0xff);
        hash[i * 4 + 3] = static_cast<uint8_t>(ctx.state[i] & 0xff);
    }
}

} // namespace

std::vector<uint8_t> sha256_raw(std::string_view data) {
    SHA256Context ctx;
    sha256_init(ctx);
    sha256_update(ctx, reinterpret_cast<const uint8_t*>(data.data()), data.size());
    std::vector<uint8_t> hash(32);
    sha256_final(ctx, hash.data());
    return hash;
}

std::string sha256_hex(std::string_view data) {
    auto hash = sha256_raw(data);
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (uint8_t b : hash) {
        oss << std::setw(2) << static_cast<unsigned int>(b);
    }
    return oss.str();
}

// ============================================================================
// 3. SHA-512 Implementation (for Ed25519)
// ============================================================================

namespace {

inline uint64_t rotr64(uint64_t x, uint64_t n) { return (x >> n) | (x << (64 - n)); }
inline uint64_t choose64(uint64_t e, uint64_t f, uint64_t g) { return (e & f) ^ (~e & g); }
inline uint64_t majority64(uint64_t a, uint64_t b, uint64_t c) { return (a & b) ^ (a & c) ^ (b & c); }
inline uint64_t sig0_64(uint64_t x) { return rotr64(x, 28) ^ rotr64(x, 34) ^ rotr64(x, 39); }
inline uint64_t sig1_64(uint64_t x) { return rotr64(x, 14) ^ rotr64(x, 18) ^ rotr64(x, 41); }
inline uint64_t theta0_64(uint64_t x) { return rotr64(x, 1) ^ rotr64(x, 8) ^ (x >> 7); }
inline uint64_t theta1_64(uint64_t x) { return rotr64(x, 19) ^ rotr64(x, 61) ^ (x >> 6); }

static const uint64_t SHA512_K[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47867871ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
};

struct SHA512Context {
    uint64_t state[8];
    uint64_t bit_count[2];
    uint8_t buffer[128];
};

void sha512_init(SHA512Context& ctx) {
    ctx.state[0] = 0x6a09e667f3bcc908ULL;
    ctx.state[1] = 0xbb67ae8584caa73bULL;
    ctx.state[2] = 0x3c6ef372fe94f82bULL;
    ctx.state[3] = 0xa54ff53a5f1d36f1ULL;
    ctx.state[4] = 0x510e527fade682d1ULL;
    ctx.state[5] = 0x9b05688c2b3e6c1fULL;
    ctx.state[6] = 0x1f83d9abfb41bd6bULL;
    ctx.state[7] = 0x5be0cd19137e2179ULL;
    ctx.bit_count[0] = ctx.bit_count[1] = 0;
}

void sha512_transform(SHA512Context& ctx, const uint8_t* data) {
    uint64_t W[80];
    for (int i = 0; i < 16; ++i) {
        W[i] = (static_cast<uint64_t>(data[i * 8]) << 56) |
               (static_cast<uint64_t>(data[i * 8 + 1]) << 48) |
               (static_cast<uint64_t>(data[i * 8 + 2]) << 40) |
               (static_cast<uint64_t>(data[i * 8 + 3]) << 32) |
               (static_cast<uint64_t>(data[i * 8 + 4]) << 24) |
               (static_cast<uint64_t>(data[i * 8 + 5]) << 16) |
               (static_cast<uint64_t>(data[i * 8 + 6]) << 8) |
               (static_cast<uint64_t>(data[i * 8 + 7]));
    }
    for (int i = 16; i < 80; ++i) {
        W[i] = theta1_64(W[i - 2]) + W[i - 7] + theta0_64(W[i - 15]) + W[i - 16];
    }

    uint64_t a = ctx.state[0], b = ctx.state[1], c = ctx.state[2], d = ctx.state[3];
    uint64_t e = ctx.state[4], f = ctx.state[5], g = ctx.state[6], h = ctx.state[7];

    for (int i = 0; i < 80; ++i) {
        uint64_t T1 = h + sig1_64(e) + choose64(e, f, g) + SHA512_K[i] + W[i];
        uint64_t T2 = sig0_64(a) + majority64(a, b, c);
        h = g; g = f; f = e; e = d + T1;
        d = c; c = b; b = a; a = T1 + T2;
    }

    ctx.state[0] += a; ctx.state[1] += b; ctx.state[2] += c; ctx.state[3] += d;
    ctx.state[4] += e; ctx.state[5] += f; ctx.state[6] += g; ctx.state[7] += h;
}

void sha512_update(SHA512Context& ctx, const uint8_t* data, size_t length) {
    size_t buffer_bytes = (ctx.bit_count[0] / 8) % 128;
    ctx.bit_count[0] += static_cast<uint64_t>(length) * 8;
    if (ctx.bit_count[0] < static_cast<uint64_t>(length) * 8) {
        ctx.bit_count[1]++;
    }

    size_t offset = 0;
    if (buffer_bytes > 0) {
        size_t needed = 128 - buffer_bytes;
        size_t to_copy = (length < needed) ? length : needed;
        std::memcpy(ctx.buffer + buffer_bytes, data, to_copy);
        buffer_bytes += to_copy;
        offset += to_copy;
        if (buffer_bytes == 128) {
            sha512_transform(ctx, ctx.buffer);
            buffer_bytes = 0;
        }
    }

    while (offset + 128 <= length) {
        sha512_transform(ctx, data + offset);
        offset += 128;
    }

    if (offset < length) {
        std::memcpy(ctx.buffer + buffer_bytes, data + offset, length - offset);
    }
}

void sha512_final(SHA512Context& ctx, uint8_t hash[64]) {
    size_t buffer_bytes = (ctx.bit_count[0] / 8) % 128;
    ctx.buffer[buffer_bytes++] = 0x80;

    if (buffer_bytes > 112) {
        std::memset(ctx.buffer + buffer_bytes, 0, 128 - buffer_bytes);
        sha512_transform(ctx, ctx.buffer);
        buffer_bytes = 0;
    }

    std::memset(ctx.buffer + buffer_bytes, 0, 112 - buffer_bytes);
    for (int i = 0; i < 8; ++i) {
        ctx.buffer[112 + i] = static_cast<uint8_t>((ctx.bit_count[1] >> (56 - i * 8)) & 0xff);
        ctx.buffer[120 + i] = static_cast<uint8_t>((ctx.bit_count[0] >> (56 - i * 8)) & 0xff);
    }
    sha512_transform(ctx, ctx.buffer);

    for (int i = 0; i < 8; ++i) {
        for (int j = 0; j < 8; ++j) {
            hash[i * 8 + j] = static_cast<uint8_t>((ctx.state[i] >> (56 - j * 8)) & 0xff);
        }
    }
}

void compute_sha512(const uint8_t* data, size_t length, uint8_t out[64]) {
    SHA512Context ctx;
    sha512_init(ctx);
    sha512_update(ctx, data, length);
    sha512_final(ctx, out);
}

} // namespace

// ============================================================================
// 4. Ed25519 Compact Field Math & Sign/Verify (TweetNaCl/Ref10 derivation)
// ============================================================================

namespace {

using gf = int64_t[16];

static const gf _0 = {0};
static const gf _1 = {1};
static const gf _d = {-0x0c13, 0x011b, -0x0b44, 0x0053, -0x0002, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000};
static const gf _2d = {-0x1826, 0x0236, -0x1688, 0x00a6, -0x0004, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000};
static const gf _I = {0xa0b0, 0x4a0e, 0x1b27, 0x45bf, 0x4016, 0x7334, 0x5a58, 0x6e69, 0x7179, 0x6a0a, 0x70bb, 0x4e6b, 0x4dfb, 0x676b, 0x2213, 0x2b83};

static const uint8_t L[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10
};

void car25519(gf o) {
    for (int i = 0; i < 16; ++i) {
        o[i] += (1LL << 16);
        int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15 ? 1 : 0)] += c - 1 + (i == 15 ? 37 * (c - 1) : 0);
        o[i] -= c << 16;
    }
}

void sel25519(gf p, gf q, int b) {
    int64_t c = ~(b - 1);
    for (int i = 0; i < 16; ++i) {
        int64_t t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

void pack25519(uint8_t* o, const gf n) {
    gf m, t;
    std::memcpy(t, n, sizeof(gf));
    car25519(t); car25519(t); car25519(t);
    for (int j = 0; j < 2; ++j) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; ++i) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = static_cast<int>((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; ++i) {
        o[2 * i] = static_cast<uint8_t>(t[i] & 0xFF);
        o[2 * i + 1] = static_cast<uint8_t>(t[i] >> 8);
    }
}

void unpack25519(gf o, const uint8_t* n) {
    for (int i = 0; i < 16; ++i) {
        o[i] = n[2 * i] + (static_cast<int64_t>(n[2 * i + 1]) << 8);
    }
    o[15] &= 0x7fff;
}

void A(gf o, const gf a, const gf b) { for (int i = 0; i < 16; ++i) o[i] = a[i] + b[i]; }
void Z(gf o, const gf a, const gf b) { for (int i = 0; i < 16; ++i) o[i] = a[i] - b[i]; }

void M(gf o, const gf a, const gf b) {
    int64_t t[31] = {0};
    for (int i = 0; i < 16; ++i) {
        for (int j = 0; j < 16; ++j) {
            t[i + j] += a[i] * b[j];
        }
    }
    for (int i = 0; i < 15; ++i) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; ++i) o[i] = t[i];
    car25519(o); car25519(o);
}

void S(gf o, const gf a) { M(o, a, a); }

void inv25519(gf o, const gf i) {
    gf c;
    std::memcpy(c, i, sizeof(gf));
    for (int a = 253; a >= 0; --a) {
        S(c, c);
        if (a != 2 && a != 4) M(c, c, i);
    }
    std::memcpy(o, c, sizeof(gf));
}

void add(gf p[4], gf q[4]) {
    gf a, b, c, d, t, e, f, g, h;
    Z(a, p[1], p[0]); Z(t, q[1], q[0]); M(a, a, t);
    A(b, p[1], p[0]); A(t, q[1], q[0]); M(b, b, t);
    M(c, p[3], q[3]); M(c, c, _2d);
    M(d, p[2], q[2]); A(d, d, d);
    Z(e, b, a); Z(f, d, c); A(g, d, c); A(h, b, a);
    M(p[0], e, f); M(p[1], h, g); M(p[2], g, f); M(p[3], e, h);
}

void cswap(gf p[4], gf q[4], int b) {
    for (int i = 0; i < 4; ++i) sel25519(p[i], q[i], b);
}

void scalarmult(gf p[4], gf q[4], const uint8_t* s) {
    std::memcpy(p[0], _0, sizeof(gf));
    std::memcpy(p[1], _1, sizeof(gf));
    std::memcpy(p[2], _1, sizeof(gf));
    std::memcpy(p[3], _0, sizeof(gf));
    for (int i = 255; i >= 0; --i) {
        int b = (s[i / 8] >> (i & 7)) & 1;
        cswap(p, q, b);
        add(q, p);
        add(p, p);
        cswap(p, q, b);
    }
}

void scalarbase(gf p[4], const uint8_t* s) {
    gf q[4];
    std::memcpy(q[0], _0, sizeof(gf));
    std::memcpy(q[1], _1, sizeof(gf));
    std::memcpy(q[2], _1, sizeof(gf));
    std::memcpy(q[3], _0, sizeof(gf));

    // Base point (X, Y, Z, T)
    static const gf Bx = {0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c, 0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53b3, 0xcd6e, 0x36d3, 0x2169};
    static const gf By = {0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666};
    std::memcpy(q[0], Bx, sizeof(gf));
    std::memcpy(q[1], By, sizeof(gf));
    M(q[3], Bx, By);

    scalarmult(p, q, s);
}

void modL(uint8_t* r, int64_t x[64]) {
    for (int i = 63; i >= 32; --i) {
        int64_t carry = 0;
        for (int j = 0; j < 32; ++j) {
            x[i - 32 + j] += carry - (x[i] * L[j]);
            carry = x[i - 32 + j] >> 8;
            x[i - 32 + j] &= 0xFF;
        }
        x[i] = 0;
    }
    int64_t carry = 0;
    for (int j = 0; j < 32; ++j) {
        int64_t val = x[j] + carry;
        r[j] = static_cast<uint8_t>(val & 0xFF);
        carry = val >> 8;
    }
}

void reduce(uint8_t* r) {
    int64_t x[64];
    for (int i = 0; i < 64; ++i) x[i] = r[i];
    std::memset(r, 0, 64);
    modL(r, x);
}

} // namespace

// ============================================================================
// 5. Crypto Public API Implementation
// ============================================================================

bool secure_equal(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        volatile uint8_t dummy = 0;
        for (size_t i = 0; i < left.size(); ++i) dummy ^= left[i];
        (void)dummy;
        return false;
    }
    uint8_t result = 0;
    for (size_t i = 0; i < left.size(); ++i) {
        result |= static_cast<uint8_t>(left[i] ^ right[i]);
    }
    return result == 0;
}

std::string create_nonce() {
    uint8_t bytes[24];
#if defined(_WIN32)
    BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t r = read(fd, bytes, sizeof(bytes));
        (void)r;
        close(fd);
    } else {
        std::random_device rd;
        for (size_t i = 0; i < sizeof(bytes); ++i) bytes[i] = static_cast<uint8_t>(rd());
    }
#endif
    return base64url_encode(bytes, sizeof(bytes));
}

std::string canonical_json(const nlohmann::json& j) {
    if (j.is_null()) return "null";
    if (j.is_boolean()) return j.get<bool>() ? "true" : "false";
    if (j.is_number()) return j.dump();
    if (j.is_string()) return j.dump();

    if (j.is_array()) {
        std::string s = "[";
        bool first = true;
        for (const auto& item : j) {
            if (!first) s.push_back(',');
            s += canonical_json(item);
            first = false;
        }
        s.push_back(']');
        return s;
    }

    if (j.is_object()) {
        std::vector<std::string> keys;
        keys.reserve(j.size());
        for (auto it = j.begin(); it != j.end(); ++it) {
            keys.push_back(it.key());
        }
        std::sort(keys.begin(), keys.end());

        std::string s = "{";
        bool first = true;
        for (const auto& k : keys) {
            if (!first) s.push_back(',');
            s += nlohmann::json(k).dump();
            s.push_back(':');
            s += canonical_json(j.at(k));
            first = false;
        }
        s.push_back('}');
        return s;
    }

    return j.dump();
}

std::string payload_hash(const nlohmann::json& payload) {
    return sha256_hex(canonical_json(payload));
}

std::string make_signing_string(
    const std::string& machine_id,
    int64_t timestamp,
    const std::string& nonce,
    const std::string& request_id,
    const std::string& hash
) {
    return machine_id + "." + std::to_string(timestamp) + "." + nonce + "." + request_id + "." + hash;
}

Identity generate_identity(const std::string& machine_id) {
    Identity id;
    id.machine_id = machine_id;

    uint8_t seed[32];
#if defined(_WIN32)
    BCryptGenRandom(nullptr, seed, sizeof(seed), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
#else
    std::random_device rd;
    for (int i = 0; i < 32; ++i) seed[i] = static_cast<uint8_t>(rd());
#endif

    uint8_t az[64];
    compute_sha512(seed, 32, az);
    az[0] &= 248;
    az[31] &= 127;
    az[31] |= 64;

    gf p[4];
    scalarbase(p, az);
    uint8_t pk[32];
    gf inv_z;
    inv25519(inv_z, p[2]);
    gf x, y;
    M(x, p[0], inv_z);
    M(y, p[1], inv_z);
    pack25519(pk, y);
    pk[31] ^= (x[0] & 1) << 7;

    id.public_key_raw.assign(pk, pk + 32);
    id.private_key_raw.assign(seed, seed + 32);
    id.private_key_raw.insert(id.private_key_raw.end(), pk, pk + 32);

    id.public_key_pem = "-----BEGIN PUBLIC KEY-----\n" + base64_encode(pk, 32) + "\n-----END PUBLIC KEY-----\n";
    id.private_key_pem = "-----BEGIN PRIVATE KEY-----\n" + base64_encode(id.private_key_raw.data(), id.private_key_raw.size()) + "\n-----END PRIVATE KEY-----\n";
    return id;
}

std::string sign_request(const Identity& identity, const std::string& value) {
    return sign_request(identity.private_key_raw, value);
}

std::string sign_request(const std::vector<uint8_t>& private_key_raw, const std::string& value) {
    if (private_key_raw.size() < 32) return "";
    const uint8_t* seed = private_key_raw.data();

    uint8_t az[64];
    compute_sha512(seed, 32, az);
    az[0] &= 248;
    az[31] &= 127;
    az[31] |= 64;

    uint8_t pk[32];
    if (private_key_raw.size() >= 64) {
        std::memcpy(pk, private_key_raw.data() + 32, 32);
    } else {
        gf p[4];
        scalarbase(p, az);
        gf inv_z, x, y;
        inv25519(inv_z, p[2]);
        M(x, p[0], inv_z);
        M(y, p[1], inv_z);
        pack25519(pk, y);
        pk[31] ^= (x[0] & 1) << 7;
    }

    // Nonce r = H(az[32..63] || msg)
    SHA512Context s_ctx;
    sha512_init(s_ctx);
    sha512_update(s_ctx, az + 32, 32);
    sha512_update(s_ctx, reinterpret_cast<const uint8_t*>(value.data()), value.size());
    uint8_t r_hash[64];
    sha512_final(s_ctx, r_hash);
    reduce(r_hash);

    gf R[4];
    scalarbase(R, r_hash);
    uint8_t sig_R[32];
    gf inv_rz, rx, ry;
    inv25519(inv_rz, R[2]);
    M(rx, R[0], inv_rz);
    M(ry, R[1], inv_rz);
    pack25519(sig_R, ry);
    sig_R[31] ^= (rx[0] & 1) << 7;

    // k = H(sig_R || pk || msg)
    sha512_init(s_ctx);
    sha512_update(s_ctx, sig_R, 32);
    sha512_update(s_ctx, pk, 32);
    sha512_update(s_ctx, reinterpret_cast<const uint8_t*>(value.data()), value.size());
    uint8_t k_hash[64];
    sha512_final(s_ctx, k_hash);
    reduce(k_hash);

    // S = (r + k * az) mod L
    int64_t x_s[64] = {0};
    for (int i = 0; i < 32; ++i) x_s[i] = r_hash[i];
    for (int i = 0; i < 32; ++i) {
        for (int j = 0; j < 32; ++j) {
            x_s[i + j] += static_cast<int64_t>(k_hash[i]) * static_cast<int64_t>(az[j]);
        }
    }
    uint8_t sig_S[32];
    modL(sig_S, x_s);

    uint8_t signature[64];
    std::memcpy(signature, sig_R, 32);
    std::memcpy(signature + 32, sig_S, 32);

    return base64url_encode(signature, 64);
}

bool verify_request(const std::vector<uint8_t>& public_key_raw, const std::string& value, const std::string& signature_base64url) {
    if (public_key_raw.size() != 32) return false;
    auto sig_bytes = base64url_decode(signature_base64url);
    if (sig_bytes.size() != 64) return false;

    // Reconstruct k = H(R || pk || msg)
    SHA512Context s_ctx;
    sha512_init(s_ctx);
    sha512_update(s_ctx, sig_bytes.data(), 32);
    sha512_update(s_ctx, public_key_raw.data(), 32);
    sha512_update(s_ctx, reinterpret_cast<const uint8_t*>(value.data()), value.size());
    uint8_t k_hash[64];
    sha512_final(s_ctx, k_hash);
    reduce(k_hash);

    // Verify S < L
    const uint8_t* sig_S = sig_bytes.data() + 32;
    for (int i = 31; i >= 0; --i) {
        if (sig_S[i] > L[i]) return false;
        if (sig_S[i] < L[i]) break;
    }

    // Check R == SB - kA
    gf SB[4];
    scalarbase(SB, sig_S);

    gf A_pt[4];
    unpack25519(A_pt[1], public_key_raw.data());
    std::memcpy(A_pt[2], _1, sizeof(gf));
    gf x, y2, u, v, v3, vxx, check;
    S(y2, A_pt[1]);
    M(u, y2, _1); Z(u, u, _1);
    M(v, _d, y2); A(v, _1, v);
    inv25519(v, v);
    M(x, u, v);
    inv25519(x, x);
    std::memcpy(A_pt[0], x, sizeof(gf));
    M(A_pt[3], A_pt[0], A_pt[1]);

    gf kA[4];
    scalarmult(kA, A_pt, k_hash);

    // Negate kA (X = -X, T = -T)
    Z(kA[0], _0, kA[0]);
    Z(kA[3], _0, kA[3]);

    add(SB, kA);

    uint8_t check_R[32];
    gf inv_z, rx, ry;
    inv25519(inv_z, SB[2]);
    M(rx, SB[0], inv_z);
    M(ry, SB[1], inv_z);
    pack25519(check_R, ry);
    check_R[31] ^= (rx[0] & 1) << 7;

    return std::memcmp(check_R, sig_bytes.data(), 32) == 0;
}

bool verify_request_pem(const std::string& public_key_pem, const std::string& value, const std::string& signature_base64url) {
    std::string b64;
    std::istringstream stream(public_key_pem);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.find("-----") != std::string::npos) continue;
        for (char c : line) {
            if (!std::isspace(static_cast<unsigned char>(c))) b64.push_back(c);
        }
    }
    auto raw = base64_decode(b64);
    if (raw.size() == 32) {
        return verify_request(raw, value, signature_base64url);
    }
    if (raw.size() >= 32) {
        // Take last 32 bytes for SPKI header wrapper
        std::vector<uint8_t> key32(raw.end() - 32, raw.end());
        return verify_request(key32, value, signature_base64url);
    }
    return false;
}

bool verify_pkce(const std::string& verifier, const std::string& challenge, const std::string& method) {
    if (method == "plain") {
        return secure_equal(verifier, challenge);
    }
    // S256 method
    std::vector<uint8_t> hash = sha256_raw(verifier);
    std::string calculated = base64url_encode(hash.data(), hash.size());
    log_info("PKCE", "verifier: '", verifier, "' -> calculated: '", calculated, "' vs challenge: '", challenge, "'");
    return secure_equal(calculated, challenge);
}

} // namespace machinebridge
