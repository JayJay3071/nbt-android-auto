/* Explicit public-fixture audit; signature validity is not trust enrollment. */
#include <mbedtls/x509_crt.h>
#include <mbedtls/md.h>
#include <mbedtls/error.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void check(int rc, const char *what) {
    char error[160];
    if (!rc) return;
    mbedtls_strerror(rc,error,sizeof(error));
    fprintf(stderr,"FAIL %s: %d %s\n",what,rc,error); exit(1);
}
int main(int argc, char **argv) {
    mbedtls_x509_crt cert, issuer;
    const mbedtls_md_info_t *md;
    unsigned char der[16384], digest[64];
    FILE *f;
    size_t n;
    int extra, rc;
    if (argc != 3 || !(f=fopen(argv[1],"rb"))) return 2;
    n=fread(der,1,sizeof(der),f); extra=fgetc(f); fclose(f);
    if (!n || extra != EOF) return 2;
    mbedtls_x509_crt_init(&cert); mbedtls_x509_crt_init(&issuer);
    check(mbedtls_x509_crt_parse_der(&cert,der,n),"legacy certificate parse");
    if (cert.raw.len != n || memcmp(cert.raw.p,der,n)) return 3;
    check(mbedtls_x509_crt_parse_file(&issuer,argv[2]),"issuer parse");
    md=mbedtls_md_info_from_type(cert.MBEDTLS_PRIVATE(sig_md));
    if (!md) return 3;
    check(mbedtls_md(md,cert.tbs.p,cert.tbs.len,digest),"TBS hash");
    rc=mbedtls_pk_verify_ext(cert.MBEDTLS_PRIVATE(sig_pk),cert.MBEDTLS_PRIVATE(sig_opts),
        &issuer.pk,cert.MBEDTLS_PRIVATE(sig_md),digest,mbedtls_md_get_size(md),
        cert.MBEDTLS_PRIVATE(sig).p,cert.MBEDTLS_PRIVATE(sig).len);
    check(rc,"original certificate signature against supplied issuer");
    digest[0] ^= 1;
    rc=mbedtls_pk_verify_ext(cert.MBEDTLS_PRIVATE(sig_pk),cert.MBEDTLS_PRIVATE(sig_opts),
        &issuer.pk,cert.MBEDTLS_PRIVATE(sig_md),digest,mbedtls_md_get_size(md),
        cert.MBEDTLS_PRIVATE(sig).p,cert.MBEDTLS_PRIVATE(sig).len);
    if (rc == 0) return 4;
    printf("CERT_COMPAT_OK raw_unchanged signature_verified tamper_rejected validity_utc=%04d-%02d-%02dT%02d:%02d:%02d/%04d-%02d-%02dT%02d:%02d:%02d\n",
        cert.valid_from.year,cert.valid_from.mon,cert.valid_from.day,cert.valid_from.hour,cert.valid_from.min,cert.valid_from.sec,
        cert.valid_to.year,cert.valid_to.mon,cert.valid_to.day,cert.valid_to.hour,cert.valid_to.min,cert.valid_to.sec);
    mbedtls_x509_crt_free(&cert); mbedtls_x509_crt_free(&issuer);
    return 0;
}
