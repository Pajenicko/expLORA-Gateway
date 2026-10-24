/**
 * mbedtls configuration for the native tests (scripts/native_mbedtls.py):
 * only what SigVerify and Sha256 use - SHA-256, big numbers, the P-256
 * curve and ECDSA. The firmware uses the ESP-IDF build of the same mbedtls
 * version (3.6.6) with its own configuration.
 */

#pragma once

#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_SHA256_C
