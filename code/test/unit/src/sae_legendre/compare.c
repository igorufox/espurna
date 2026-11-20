/*
 * Host test of the Legendre symbol of the ESP8266 package's wpa_supplicant (crypto_mbedtls-bignum.c, 19.11): the
 * binary Jacobi version against the SDK's exponentiation, on the mbedtls of the package. The two functions are cut
 * out of the package file by the build line (run.sh) into legendre.inc.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/bignum.h"

struct crypto_bignum;

#include "legendre.inc"

static unsigned long long state = 0x9E3779B97F4A7C15ull;

static int rnd(void* ctx, unsigned char* out, size_t len) {
    (void) ctx;
    for (size_t i = 0; i < len; ++i) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        out[i] = (unsigned char) state;
    }
    return 0;
}

int main(void) {
    static const char* primes[] = {
        /* P-256, P-384 field primes (SAE groups 19, 20) and a small one */
        "FFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFF",
        "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEFFFFFFFF0000000000000000FFFFFFFF",
        "1F",
    };

    int failures = 0;
    long checks = 0;
    for (size_t pi = 0; pi < sizeof(primes) / sizeof(primes[0]); ++pi) {
        mbedtls_mpi p, a;
        mbedtls_mpi_init(&p);
        mbedtls_mpi_init(&a);
        mbedtls_mpi_read_string(&p, 16, primes[pi]);
        const size_t bytes = mbedtls_mpi_size(&p);

        for (int i = 0; i < 20000; ++i) {
            if (i == 0) {
                mbedtls_mpi_lset(&a, 0);
            } else if (i == 1) {
                mbedtls_mpi_copy(&a, &p);
            } else if (i == 2) {
                mbedtls_mpi_lset(&a, 1);
            } else {
                mbedtls_mpi_fill_random(&a, bytes + (i % 3), rnd, NULL);
            }

            const int fast = crypto_bignum_legendre((const struct crypto_bignum*) &a, (const struct crypto_bignum*) &p);
            const int slow = crypto_bignum_legendre_exp((const struct crypto_bignum*) &a, (const struct crypto_bignum*) &p);
            ++checks;
            if (fast != slow) {
                if (failures < 10) {
                    printf("prime %zu case %d: jacobi %d exp %d\n", pi, i, fast, slow);
                }
                ++failures;
            }
        }

        mbedtls_mpi_free(&a);
        mbedtls_mpi_free(&p);
    }

    printf("%ld checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
