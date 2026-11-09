// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// AACryptor interface: Copyright (c) 2026 nanaisu - NBT-AAD
// Required Notice: Copyright (c) 2026 nanaisu - NBT-AAD (https://github.com/Sq00ky/Supra-iDrive6-Android-Auto)
// New backend: bounded memory BIOs, TLS 1.2, required peer verification.
// No embedded keys and no system TLS library replacement.
#include "aa_cryptor.hpp"
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/error.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace nbt {
static const size_t kQueueLimit = 256 * 1024;

struct AACryptor::State {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    mbedtls_x509_crt cert, ca;
    mbedtls_pk_context key;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context rng;
    std::vector<uint8_t> input, output;
    size_t consumed;
    bool complete, failed;
    bool pinned, anyLeaf;
    unsigned char peer_digest[32];
    State() : consumed(0), complete(false), failed(false), pinned(false), anyLeaf(false) {
        mbedtls_ssl_init(&ssl); mbedtls_ssl_config_init(&config);
        mbedtls_x509_crt_init(&cert); mbedtls_x509_crt_init(&ca);
        mbedtls_pk_init(&key); mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&rng);
    }
    static int verify(void* context, mbedtls_x509_crt* cert, int depth, uint32_t* flags) {
        State* s = static_cast<State*>(context);
        {   // diagnostics (car 2026-11-07: "Certificate verification failed" with a phone that worked before)
            char subj[160] = "", iss[160] = "";
            mbedtls_x509_dn_gets(subj, sizeof(subj), &cert->subject);
            mbedtls_x509_dn_gets(iss, sizeof(iss), &cert->issuer);
            printf("[TLS] depth %d flags 0x%x subject '%s' issuer '%s' valid %04d-%02d-%02d %02d:%02d .. %04d-%02d-%02d %02d:%02d\n",
                   depth, (unsigned)*flags, subj, iss, cert->valid_from.year, cert->valid_from.mon, cert->valid_from.day,
                   cert->valid_from.hour, cert->valid_from.min, cert->valid_to.year, cert->valid_to.mon, cert->valid_to.day,
                   cert->valid_to.hour, cert->valid_to.min);
        }
        if (depth == 0 && s->pinned && !s->anyLeaf) {
            unsigned char digest[32];
            if (mbedtls_sha256(cert->raw.p, cert->raw.len, digest, 0) != 0 ||
                memcmp(digest, s->peer_digest, sizeof(digest)) != 0)
                *flags |= MBEDTLS_X509_BADCERT_OTHER;
        }
        /* Never clear chain, expiry, key-usage or other verification failures. */
        return 0;
    }
    ~State() {
        mbedtls_ssl_free(&ssl); mbedtls_ssl_config_free(&config);
        mbedtls_x509_crt_free(&cert); mbedtls_x509_crt_free(&ca);
        mbedtls_pk_free(&key); mbedtls_ctr_drbg_free(&rng);
        mbedtls_entropy_free(&entropy);
    }
    static int send(void* context, const unsigned char* data, size_t n) {
        State* s = static_cast<State*>(context);
        if (n > kQueueLimit - s->output.size()) return MBEDTLS_ERR_SSL_ALLOC_FAILED;
        s->output.insert(s->output.end(), data, data + n);
        return static_cast<int>(n);
    }
    static int receive(void* context, unsigned char* data, size_t n) {
        State* s = static_cast<State*>(context);
        size_t available = s->input.size() - s->consumed;
        if (!available) return MBEDTLS_ERR_SSL_WANT_READ;
        if (n > available) n = available;
        memcpy(data, &s->input[s->consumed], n);
        s->consumed += n;
        return static_cast<int>(n);
    }
};

AACryptor::AACryptor() : state_(NULL) { error_[0] = 0; }
AACryptor::~AACryptor() { cleanup(); }
void AACryptor::cleanup() { delete state_; state_ = NULL; }
bool AACryptor::failure(const char* operation, int code) {
    char detail[128];
    mbedtls_strerror(code, detail, sizeof(detail));
    snprintf(error_, sizeof(error_), "%s: %d %s", operation, code, detail);
    if (state_) { state_->failed = true; state_->complete = false; }
    return false;
}

bool AACryptor::init() {
    cleanup(); error_[0] = 0;
    const char* cert = getenv("NBT_AA_TLS_CERT");
    const char* key = getenv("NBT_AA_TLS_KEY");
    const char* ca = getenv("NBT_AA_TLS_CA");
    const char* peer = getenv("NBT_AA_TLS_PEER");
    if (!cert || !*cert || !key || !*key || !ca || !*ca || !peer || !*peer) {
        snprintf(error_, sizeof(error_), "NBT_AA_TLS_CERT/KEY/CA/PEER are required (no fallback identity)");
        return false;
    }
    state_ = new State;
    /* "chain": any phone whose certificate chains to NBT_AA_TLS_CA (the Android Auto root), with validity and usage
     * checks, but without a per-app leaf pin, so other phones and Android Auto app updates keep working. */
    const bool chainOnly = !strcmp(peer, "chain");
    if (chainOnly) state_->pinned = true;   /* same TLS setup as a pin (no DNS name), the callback skips the digest */
    if (chainOnly) state_->anyLeaf = true;
    else if (!strncmp(peer, "sha256:", 7)) {
        if (strlen(peer + 7) != 64) return failure("peer fingerprint length", MBEDTLS_ERR_SSL_BAD_INPUT_DATA);
        for (unsigned i = 0; i < 32; ++i) {
            unsigned value = 0;
            for (unsigned j = 0; j < 2; ++j) {
                char c = peer[7 + i*2 + j];
                unsigned digit;
                if (c >= '0' && c <= '9') digit = c-'0';
                else if (c >= 'a' && c <= 'f') digit = c-'a'+10;
                else if (c >= 'A' && c <= 'F') digit = c-'A'+10;
                else return failure("peer fingerprint encoding", MBEDTLS_ERR_SSL_BAD_INPUT_DATA);
                value = value*16 + digit;
            }
            state_->peer_digest[i] = value;
        }
        state_->pinned = true;
    }
    int rc = static_cast<int>(psa_crypto_init());
    if (rc) return failure("PSA init", rc);
    static const unsigned char seedLabel[] = "NBT QNX AA TLS";
    rc = mbedtls_ctr_drbg_seed(&state_->rng, mbedtls_entropy_func, &state_->entropy,
                               seedLabel, sizeof(seedLabel) - 1);
    if (rc) return failure("OS entropy", rc);
    rc = mbedtls_x509_crt_parse_file(&state_->cert, cert);
    if (rc) return failure("own certificate", rc);
    rc = mbedtls_pk_parse_keyfile(&state_->key, key, NULL, mbedtls_ctr_drbg_random, &state_->rng);
    if (rc) return failure("own key", rc);
    rc = mbedtls_pk_check_pair(&state_->cert.pk, &state_->key, mbedtls_ctr_drbg_random, &state_->rng);
    if (rc) return failure("own certificate/key match", rc);
    rc = mbedtls_x509_crt_parse_file(&state_->ca, ca);
    if (rc) return failure("trust chain", rc);
    rc = mbedtls_ssl_config_defaults(&state_->config, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc) return failure("TLS defaults", rc);
    mbedtls_ssl_conf_min_tls_version(&state_->config, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&state_->config, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_authmode(&state_->config, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_verify(&state_->config, State::verify, state_);
    mbedtls_ssl_conf_rng(&state_->config, mbedtls_ctr_drbg_random, &state_->rng);
    mbedtls_ssl_conf_ca_chain(&state_->config, &state_->ca, NULL);
    rc = mbedtls_ssl_conf_own_cert(&state_->config, &state_->cert, &state_->key);
    if (rc) return failure("own TLS identity", rc);
    rc = mbedtls_ssl_setup(&state_->ssl, &state_->config);
    if (rc) return failure("TLS setup", rc);
    /* Explicit NULL is required by Mbed TLS 3.6 for non-DNS identities.
     * The verification callback enforces the configured leaf fingerprint. */
    rc = mbedtls_ssl_set_hostname(&state_->ssl, state_->pinned ? NULL : peer);
    if (rc) return failure("peer name", rc);
    mbedtls_ssl_set_bio(&state_->ssl, state_, State::send, State::receive, NULL);
    return true;
}

bool AACryptor::isHandshakeComplete() const { return state_ && state_->complete && !state_->failed; }
size_t AACryptor::pendingOut() const { return state_ ? state_->output.size() : 0; }
bool AACryptor::feed(const uint8_t* data, size_t n) {
    if (!state_ || state_->failed || (!data && n)) return false;
    if (n > kQueueLimit - (state_->input.size() - state_->consumed))
        return failure("input limit", MBEDTLS_ERR_SSL_ALLOC_FAILED);
    if (state_->consumed) {
        state_->input.erase(state_->input.begin(), state_->input.begin() + state_->consumed);
        state_->consumed = 0;
    }
    if (n) state_->input.insert(state_->input.end(), data, data + n);
    return true;
}
bool AACryptor::handshake(std::vector<uint8_t>& out) {
    out.clear();
    if (!state_ || state_->failed || state_->complete) return false;
    int rc = mbedtls_ssl_handshake(&state_->ssl);
    out.swap(state_->output);
    if (rc == 0) {
        if (mbedtls_ssl_get_verify_result(&state_->ssl) != 0)
            return failure("peer verification", MBEDTLS_ERR_X509_CERT_VERIFY_FAILED);
        state_->complete = true;
        return true;
    }
    if (rc == MBEDTLS_ERR_SSL_WANT_READ) return true;
    return failure("TLS handshake", rc);
}
bool AACryptor::startHandshake(std::vector<uint8_t>& out) { return handshake(out); }
bool AACryptor::processHandshakeData(const uint8_t* data, size_t n, std::vector<uint8_t>& out) {
    out.clear();
    return feed(data, n) && handshake(out);
}
bool AACryptor::encrypt(const uint8_t* data, size_t n, std::vector<uint8_t>& out) {
    out.clear();
    if (!isHandshakeComplete() || !data || !n || n > 16384) return false;
    int rc = mbedtls_ssl_write(&state_->ssl, data, n);
    if (rc != static_cast<int>(n)) return failure("TLS write", rc);
    out.swap(state_->output);
    return !out.empty();
}
bool AACryptor::decrypt(const uint8_t* data, size_t n, std::vector<uint8_t>& out) {
    out.clear();
    if (!isHandshakeComplete() || !feed(data, n)) return false;
    unsigned char buffer[16384];
    for (;;) {
        int rc = mbedtls_ssl_read(&state_->ssl, buffer, sizeof(buffer));
        if (rc == MBEDTLS_ERR_SSL_WANT_READ) break;
        if (rc <= 0 || static_cast<size_t>(rc) > kQueueLimit - out.size()) {
            out.clear();
            return failure("TLS read", rc ? rc : MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY);
        }
        out.insert(out.end(), buffer, buffer + rc);
    }
    return !out.empty();
}
}
