#ifndef AES128_H
#define AES128_H
#include <stdint.h>
/* Decrypt one 16-byte block with a 16-byte key (AES-128, ECB). */
void aes128_decrypt_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
#endif
