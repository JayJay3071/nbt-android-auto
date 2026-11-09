/* TLS feasibility test for QNX x86. Uses generated lab certificates only.
 * Both peers and deliberately fragmented transport live in this process.
 * This is not Android Auto authentication or an AA receiver.
 */
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/error.h>
#include <mbedtls/aes.h>
#include <mbedtls/gcm.h>
#include <mbedtls/sha256.h>
#include <mbedtls/rsa.h>
#include <psa/crypto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct peer {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    mbedtls_x509_crt ca, cert;
    mbedtls_pk_context key;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context rng;
    struct peer *other;
    unsigned char input[65536];
    size_t used;
    unsigned sends, receives;
} peer;

static void fail(const char *operation, int code) {
    char error[192];
    mbedtls_strerror(code, error, sizeof(error));
    fprintf(stderr, "FAIL %s code=%d %s\n", operation, code, error);
    exit(1);
}
static void require(int yes, const char *operation) {
    if (!yes) fail(operation, -1);
}
static void check(int result, const char *operation) {
    if (result != 0) fail(operation, result);
}

static int send_fragment(void *context, const unsigned char *data, size_t size) {
    peer *p = (peer *)context;
    size_t n = size < 37 ? size : 37;
    if (n > sizeof(p->other->input) - p->other->used) return MBEDTLS_ERR_SSL_WANT_WRITE;
    memcpy(p->other->input + p->other->used, data, n);
    p->other->used += n;
    ++p->sends;
    return (int)n;
}
static int receive_fragment(void *context, unsigned char *data, size_t size) {
    peer *p = (peer *)context;
    size_t n = size < 29 ? size : 29;
    if (!p->used) return MBEDTLS_ERR_SSL_WANT_READ;
    if (n > p->used) n = p->used;
    memcpy(data, p->input, n);
    memmove(p->input, p->input + n, p->used - n);
    p->used -= n;
    ++p->receives;
    return (int)n;
}

static void initialize(peer *p, int server, const char *hostname) {
    const char *label = server ? "qnx-lab-server" : "qnx-lab-client";
    static const int suites[] = { MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256, 0 };
    mbedtls_ssl_init(&p->ssl);
    mbedtls_ssl_config_init(&p->config);
    mbedtls_x509_crt_init(&p->ca);
    mbedtls_x509_crt_init(&p->cert);
    mbedtls_pk_init(&p->key);
    mbedtls_entropy_init(&p->entropy);
    mbedtls_ctr_drbg_init(&p->rng);
    check(mbedtls_ctr_drbg_seed(&p->rng, mbedtls_entropy_func, &p->entropy,
                               (const unsigned char *)label, strlen(label)), "OS entropy seed");
    check(mbedtls_x509_crt_parse_file(&p->ca, "lab-ca.pem"), "CA parse");
    check(mbedtls_x509_crt_parse_file(&p->cert, server ? "lab-server.pem" : "lab-client.pem"), "cert parse");
    check(mbedtls_pk_parse_keyfile(&p->key, server ? "lab-server-key.pem" : "lab-client-key.pem",
                                  NULL, mbedtls_ctr_drbg_random, &p->rng), "key parse");
    check(mbedtls_ssl_config_defaults(&p->config,
          server ? MBEDTLS_SSL_IS_SERVER : MBEDTLS_SSL_IS_CLIENT,
          MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT), "TLS defaults");
    mbedtls_ssl_conf_min_tls_version(&p->config, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&p->config, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_ciphersuites(&p->config, suites);
    mbedtls_ssl_conf_authmode(&p->config, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_rng(&p->config, mbedtls_ctr_drbg_random, &p->rng);
    mbedtls_ssl_conf_ca_chain(&p->config, &p->ca, NULL);
    check(mbedtls_ssl_conf_own_cert(&p->config, &p->cert, &p->key), "own certificate");
    check(mbedtls_ssl_setup(&p->ssl, &p->config), "TLS setup");
    if (!server) check(mbedtls_ssl_set_hostname(&p->ssl, hostname), "hostname");
    mbedtls_ssl_set_bio(&p->ssl, p, send_fragment, receive_fragment, NULL);
}

static void release(peer *p) {
    mbedtls_ssl_free(&p->ssl);
    mbedtls_ssl_config_free(&p->config);
    mbedtls_x509_crt_free(&p->ca);
    mbedtls_x509_crt_free(&p->cert);
    mbedtls_pk_free(&p->key);
    mbedtls_ctr_drbg_free(&p->rng);
    mbedtls_entropy_free(&p->entropy);
    free(p);
}
#ifndef NBT_TLS_PEER_ONLY
static int handshake(peer *client, peer *server) {
    int c = MBEDTLS_ERR_SSL_WANT_READ, s = MBEDTLS_ERR_SSL_WANT_READ;
    unsigned step;
    for (step = 0; step < 50000; ++step) {
        if (c) c = mbedtls_ssl_handshake(&client->ssl);
        if (c && c != MBEDTLS_ERR_SSL_WANT_READ && c != MBEDTLS_ERR_SSL_WANT_WRITE) return c;
        if (s) s = mbedtls_ssl_handshake(&server->ssl);
        if (s && s != MBEDTLS_ERR_SSL_WANT_READ && s != MBEDTLS_ERR_SSL_WANT_WRITE) return s;
        if (!c && !s) return 0;
    }
    return MBEDTLS_ERR_SSL_TIMEOUT;
}

static void exchange(peer *sender, peer *receiver, size_t size) {
    unsigned char plain[16000], decoded[16000];
    size_t i, have = 0;
    unsigned step;
    int n;
    require(size <= sizeof(plain), "payload bound");
    for (i = 0; i < size; ++i) plain[i] = (unsigned char)(i * 13 + size);
    n = mbedtls_ssl_write(&sender->ssl, plain, size);
    require(n == (int)size, "encrypted write");
    for (step = 0; have < size && step < 50000; ++step) {
        n = mbedtls_ssl_read(&receiver->ssl, decoded + have, size - have);
        if (n == MBEDTLS_ERR_SSL_WANT_READ) continue;
        if (n <= 0) fail("encrypted read", n);
        have += (size_t)n;
    }
    require(have == size && memcmp(plain, decoded, size) == 0, "binary payload roundtrip");
}

#ifdef NBT_TLS_BACKEND_TEST
#include "aa_cryptor.hpp"
#include <vector>

static void test_peer_pin(void) {
    mbedtls_x509_crt certificate;
    unsigned char digest[32];
    char pin[72];
    mbedtls_x509_crt_init(&certificate);
    check(mbedtls_x509_crt_parse_file(&certificate, "lab-server.pem"), "pin fixture");
    check(mbedtls_sha256(certificate.raw.p, certificate.raw.len, digest, 0), "pin hash");
    strcpy(pin,"sha256:");
    for (unsigned i = 0; i < 32; ++i) sprintf(pin+7+i*2,"%02x",digest[i]);
    mbedtls_x509_crt_free(&certificate);
    /* modes: 0 right pin, 1 wrong pin, 2 right pin but wrong trust chain,
     * 3 "chain" (any leaf of the trusted root), 4 "chain" with a wrong trust chain */
    for (unsigned mode = 0; mode < 5; ++mode) {
        char selected[72]; strcpy(selected,pin);
        if (mode == 1) selected[7] = selected[7] == '0' ? '1' : '0';
        if (mode >= 3) strcpy(selected,"chain");
        setenv("NBT_AA_TLS_PEER",selected,1);
        setenv("NBT_AA_TLS_CA",mode == 2 || mode == 4 ? "lab-client.pem" : "lab-ca.pem",1);
        nbt::AACryptor crypt;
        require(crypt.init(),crypt.lastError());
        peer *server = (peer*)calloc(1,sizeof(peer)), *carrier = (peer*)calloc(1,sizeof(peer));
        require(server && carrier,"pin test allocation");
        server->other = carrier; initialize(server,1,NULL);
        std::vector<uint8_t> wire;
        bool ok = crypt.startHandshake(wire);
        int server_result = MBEDTLS_ERR_SSL_WANT_READ;
        for (unsigned step = 0; step < 50000 && ok; ++step) {
            require(wire.size() <= sizeof(server->input)-server->used,"pin wire bound");
            if (!wire.empty()) memcpy(server->input+server->used,&wire[0],wire.size());
            server->used += wire.size(); wire.clear();
            if (server_result) server_result = mbedtls_ssl_handshake(&server->ssl);
            require(server_result == 0 || server_result == MBEDTLS_ERR_SSL_WANT_READ,"pin server handshake");
            if (carrier->used) ok = crypt.processHandshakeData(carrier->input,carrier->used,wire);
            carrier->used = 0;
            if (!server_result && crypt.isHandshakeComplete()) break;
        }
        printf("AA_TLS_PIN_CASE mode=%u ok=%d server=%d complete=%d detail=%s\n",
            mode,ok,server_result,crypt.isHandshakeComplete(),crypt.lastError());
        require(mode == 0 || mode == 3 ? ok && !server_result && crypt.isHandshakeComplete() :
            !ok && !crypt.isHandshakeComplete(),"pin/chain enforcement");
        release(server); free(carrier);
    }
    const char* invalid[] = {"sha256:","sha256:00", "sha256:zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz"};
    for (unsigned i=0;i<3;++i) {
        setenv("NBT_AA_TLS_PEER",invalid[i],1); nbt::AACryptor crypt;
        require(!crypt.init(),"malformed pin rejected");
    }
    setenv("NBT_AA_TLS_CA","lab-ca.pem",1);
    setenv("NBT_AA_TLS_PEER","qnx-aa-lab.local",1);
    puts("AA_TLS_PIN_OK matching mismatch malformed chain-still-required chain-mode-any-leaf chain-mode-wrong-root");
}

static void test_receiver_backend(void) {
    nbt::AACryptor crypt;
    unsetenv("NBT_AA_TLS_CERT");
    require(!crypt.init(), "missing identity rejected");
    setenv("NBT_AA_TLS_CERT", "lab-client.pem", 1);
    setenv("NBT_AA_TLS_KEY", "lab-client-key.pem", 1);
    setenv("NBT_AA_TLS_CA", "lab-ca.pem", 1);
    setenv("NBT_AA_TLS_PEER", "qnx-aa-lab.local", 1);
    setenv("NBT_AA_TLS_KEY", "lab-server-key.pem", 1);
    require(!crypt.init(), "mismatched own certificate/key rejected");
    setenv("NBT_AA_TLS_KEY", "lab-client-key.pem", 1);
    require(crypt.init(), crypt.lastError());
    peer *server = (peer *)calloc(1, sizeof(peer));
    peer *carrier = (peer *)calloc(1, sizeof(peer));
    require(server && carrier, "backend peers");
    server->other = carrier;
    initialize(server, 1, NULL);
    std::vector<uint8_t> wire, plain;
    require(crypt.startHandshake(wire), crypt.lastError());
    int server_result = MBEDTLS_ERR_SSL_WANT_READ;
    for (unsigned step = 0; step < 50000; ++step) {
        require(wire.size() <= sizeof(server->input) - server->used, "server buffer bound");
        if (!wire.empty()) memcpy(server->input + server->used, &wire[0], wire.size());
        server->used += wire.size(); wire.clear();
        if (server_result) server_result = mbedtls_ssl_handshake(&server->ssl);
        require(server_result == 0 || server_result == MBEDTLS_ERR_SSL_WANT_READ, "backend TLS server");
        for (size_t offset = 0; offset < carrier->used;) {
            size_t n = carrier->used - offset;
            if (n > 29) n = 29;
            std::vector<uint8_t> response;
            require(crypt.processHandshakeData(carrier->input + offset, n, response), crypt.lastError());
            wire.insert(wire.end(), response.begin(), response.end());
            offset += n;
        }
        carrier->used = 0;
        if (!server_result && crypt.isHandshakeComplete()) break;
    }
    require(!server_result && crypt.isHandshakeComplete(), "backend handshake complete");
    std::vector<uint8_t> payload(4096), decoded;
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = (uint8_t)(i * 17);
    require(crypt.encrypt(&payload[0], payload.size(), wire), crypt.lastError());
    require(wire.size() <= sizeof(server->input) - server->used, "backend wire bound");
    memcpy(server->input + server->used, &wire[0], wire.size()); server->used += wire.size();
    unsigned char buffer[4096];
    int n = mbedtls_ssl_read(&server->ssl, buffer, sizeof(buffer));
    require(n == 4096 && memcmp(buffer, &payload[0], 4096) == 0, "backend encryption");
    require(mbedtls_ssl_write(&server->ssl, &payload[0], payload.size()) == 4096, "backend response");
    for (size_t offset = 0; offset < carrier->used;) {
        size_t part = carrier->used - offset;
        if (part > 13) part = 13;
        crypt.decrypt(carrier->input + offset, part, plain);
        require(crypt.isHandshakeComplete(), crypt.lastError());
        decoded.insert(decoded.end(), plain.begin(), plain.end()); offset += part;
    }
    require(decoded == payload, "backend fragmented decryption"); carrier->used = 0;
    require(mbedtls_ssl_write(&server->ssl, &payload[0], payload.size()) == 4096, "backend tamper response");
    carrier->input[carrier->used - 1] ^= 1;
    require(!crypt.decrypt(carrier->input, carrier->used, plain) && plain.empty() &&
            !crypt.isHandshakeComplete(), "backend tamper failure is terminal");
    release(server); free(carrier);
    require(crypt.init(), "backend reinitialize");
    std::vector<uint8_t> oversized(256 * 1024 + 1, 0);
    require(!crypt.processHandshakeData(&oversized[0], oversized.size(), wire) && wire.empty(),
            "backend queue limit");
    puts("AA_MBEDTLS_BACKEND_OK identity-required fragmented-mTLS roundtrip tamper queue-limit");
    test_peer_pin();
}
#endif

int main(void) {
    peer *client, *server;
    size_t sizes[] = {1, 255, 4096, 16000};
    unsigned i;
    int rc;
    unsigned char decoded[16];
    alarm(60);
    setvbuf(stdout, NULL, _IOLBF, 0);
    check(mbedtls_aes_self_test(0), "AES known-answer tests");
    check(mbedtls_gcm_self_test(0), "GCM known-answer tests");
    check(mbedtls_sha256_self_test(0), "SHA256 known-answer tests");
    check(mbedtls_rsa_self_test(0), "RSA known-answer tests");
    check((int)psa_crypto_init(), "PSA initialize");
    puts("CRYPTO_SELFTESTS_OK AES GCM SHA256 RSA");
    client = (peer *)calloc(1, sizeof(*client)); server = (peer *)calloc(1, sizeof(*server));
    require(client && server, "allocate peers");
    client->other = server; server->other = client;
    initialize(client, 0, "qnx-aa-lab.local"); initialize(server, 1, NULL);
    check(handshake(client, server), "mutually authenticated TLS handshake");
    require(mbedtls_ssl_get_verify_result(&client->ssl) == 0 &&
            mbedtls_ssl_get_verify_result(&server->ssl) == 0, "both certificate chains");
    require(strcmp(mbedtls_ssl_get_version(&client->ssl), "TLSv1.2") == 0, "TLS version");
    printf("TLS12_MTLS_OK cipher=%s\n", mbedtls_ssl_get_ciphersuite(&client->ssl));
    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        exchange(client, server, sizes[i]); exchange(server, client, sizes[i]);
    }
    printf("TLS_BINARY_ROUNDTRIP_OK directions=2 sizes=1,255,4096,16000 fragments=%u/%u\n",
           client->sends + server->sends, client->receives + server->receives);
    require(mbedtls_ssl_write(&client->ssl, (const unsigned char *)"tamper", 6) == 6, "tamper setup");
    require(server->used > 0, "encrypted record queued");
    server->input[server->used - 1] ^= 1;
    rc = mbedtls_ssl_read(&server->ssl, decoded, sizeof(decoded));
    require(rc == MBEDTLS_ERR_SSL_INVALID_MAC, "tampered record rejected");
    puts("TLS_TAMPER_REJECTED");
    release(client); release(server);

    client = (peer *)calloc(1, sizeof(*client)); server = (peer *)calloc(1, sizeof(*server));
    require(client && server, "allocate negative-test peers");
    client->other = server; server->other = client;
    initialize(client, 0, "wrong-host.local"); initialize(server, 1, NULL);
    rc = handshake(client, server);
    require(rc != 0 && (mbedtls_ssl_get_verify_result(&client->ssl) &
                       MBEDTLS_X509_BADCERT_CN_MISMATCH), "wrong hostname rejected");
    puts("TLS_WRONG_HOST_REJECTED");
    release(client); release(server);
#ifdef NBT_TLS_BACKEND_TEST
    test_receiver_backend();
#endif
    puts("QNX_TLS_MEMORY_TEST_OK");
    return 0;
}
#endif /* NBT_TLS_PEER_ONLY */
