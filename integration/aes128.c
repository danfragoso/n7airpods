/* aes128.c — AES-128 single-block decryption (ECB), table-free (tables are generated once).
 * Used to decode the encrypted tail of AirPods proximity advertisements (exact battery levels) with
 * the ENC_KEY the AirPods hand over on the AAP channel. Constant-time is irrelevant here: the key is
 * the user's own, the data is a 16-byte advertisement, and nothing is exposed to an attacker.
 * Verified against the FIPS-197 appendix C.1 vector (tools/test_aes128.c). */
#include <stdint.h>
#include "aes128.h"

static uint8_t s_sbox[256], s_rsbox[256];
static uint8_t s_ready;

static uint8_t rotl8(uint8_t x, int s) { return (uint8_t)((x << s) | (x >> (8 - s))); }
static uint8_t xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }
static uint8_t mul(uint8_t a, uint8_t b)
{
    uint8_t r = 0;
    while (b) { if (b & 1) r ^= a; a = xt(a); b >>= 1; }
    return r;
}

static void tables(void)
{
    uint8_t p = 1, q = 1;
    do {
        p = (uint8_t)(p ^ (uint8_t)(p << 1) ^ ((p & 0x80) ? 0x1b : 0));      /* p *= 3 in GF(2^8)  */
        q = (uint8_t)(q ^ (uint8_t)(q << 1));                                /* q /= 3             */
        q = (uint8_t)(q ^ (uint8_t)(q << 2));
        q = (uint8_t)(q ^ (uint8_t)(q << 4));
        if (q & 0x80) q ^= 0x09;
        s_sbox[p] = (uint8_t)(q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4) ^ 0x63);
    } while (p != 1);
    s_sbox[0] = 0x63;
    for (int i = 0; i < 256; i++) s_rsbox[s_sbox[i]] = (uint8_t)i;
    s_ready = 1;
}

static void expand_key(const uint8_t key[16], uint8_t rk[176])
{
    for (int i = 0; i < 16; i++) rk[i] = key[i];
    uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4] = { rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1] };
        if ((i & 15) == 0) {
            uint8_t u = t[0];
            t[0] = (uint8_t)(s_sbox[t[1]] ^ rcon); t[1] = s_sbox[t[2]]; t[2] = s_sbox[t[3]]; t[3] = s_sbox[u];
            rcon = xt(rcon);
        }
        for (int j = 0; j < 4; j++) rk[i + j] = (uint8_t)(rk[i - 16 + j] ^ t[j]);
    }
}

static void add_rk(uint8_t s[16], const uint8_t *rk) { for (int i = 0; i < 16; i++) s[i] ^= rk[i]; }

static void inv_sub_shift(uint8_t s[16])
{
    uint8_t t[16];
    /* state is column-major: s[r + 4c]. InvShiftRows moves row r right by r columns. */
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            t[r + 4 * ((c + r) & 3)] = s_rsbox[s[r + 4 * c]];
    for (int i = 0; i < 16; i++) s[i] = t[i];
}

static void inv_mix(uint8_t s[16])
{
    for (int c = 0; c < 4; c++) {
        uint8_t a0 = s[4 * c], a1 = s[4 * c + 1], a2 = s[4 * c + 2], a3 = s[4 * c + 3];
        s[4 * c]     = (uint8_t)(mul(a0, 14) ^ mul(a1, 11) ^ mul(a2, 13) ^ mul(a3, 9));
        s[4 * c + 1] = (uint8_t)(mul(a0, 9) ^ mul(a1, 14) ^ mul(a2, 11) ^ mul(a3, 13));
        s[4 * c + 2] = (uint8_t)(mul(a0, 13) ^ mul(a1, 9) ^ mul(a2, 14) ^ mul(a3, 11));
        s[4 * c + 3] = (uint8_t)(mul(a0, 11) ^ mul(a1, 13) ^ mul(a2, 9) ^ mul(a3, 14));
    }
}

void aes128_decrypt_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    uint8_t rk[176], s[16];
    if (!s_ready) tables();
    expand_key(key, rk);
    for (int i = 0; i < 16; i++) s[i] = in[i];
    add_rk(s, rk + 160);
    for (int round = 9; round >= 1; round--) {
        inv_sub_shift(s);
        add_rk(s, rk + 16 * round);
        inv_mix(s);
    }
    inv_sub_shift(s);
    add_rk(s, rk);
    for (int i = 0; i < 16; i++) out[i] = s[i];
    for (int i = 0; i < 176; i++) rk[i] = 0;            /* do not leave the schedule on the stack */
}
