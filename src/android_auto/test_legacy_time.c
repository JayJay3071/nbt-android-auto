/* Test the private Mbed TLS legacy-UTCTime overlay, never vehicle software. */
#include <mbedtls/x509_crt.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
extern int mbedtls_x509_get_time(unsigned char **, const unsigned char *, mbedtls_x509_time *);
static void expect(const char *text, int year, int mon, int day, int hour, int min, int sec) {
    unsigned char encoded[64], original[64], *p = encoded;
    mbedtls_x509_time result;
    size_t n = strlen(text);
    int rc;
    encoded[0] = 0x17; encoded[1] = n; memcpy(encoded+2, text, n);
    memcpy(original, encoded, n+2);
    rc = mbedtls_x509_get_time(&p, encoded+n+2, &result);
    if (memcmp(original,encoded,n+2) ||
        (year == 0 ? rc == 0 : rc != 0 || p != encoded+n+2 ||
        result.year != year || result.mon != mon || result.day != day ||
        result.hour != hour || result.min != min || result.sec != sec)) {
        fprintf(stderr,"Legacy time test failed: %s rc=%d\n",text,rc); exit(1);
    }
}
int main(void) {
    expect("140704000000-0700",2014,7,4,7,0,0);
    expect("480801102123-0700",2048,8,1,17,21,23);
    expect("240301001501+0130",2024,2,29,22,45,1);
    expect("230301001501+0130",2023,2,28,22,45,1);
    expect("000101000000+0001",1999,12,31,23,59,0);
    expect("991231235959-0001",2000,1,1,0,0,59);
    expect("240229235959-0030",2024,3,1,0,29,59);
    expect("491231235959-0001",2050,1,1,0,0,59);
    expect("140704000000Z",2014,7,4,0,0,0);
    expect("140704000000+0000",2014,7,4,0,0,0);
    expect("140704000000-2400",0,0,0,0,0,0);
    expect("140704000000+1260",0,0,0,0,0,0);
    expect("140704000000+0a00",0,0,0,0,0,0);
    expect("140704000000x0700",0,0,0,0,0,0);
    expect("230229000000-0700",0,0,0,0,0,0);
    expect("140704240000-0700",0,0,0,0,0,0);
    expect("1407040000-0700",0,0,0,0,0,0);
    expect("140704000000-070",0,0,0,0,0,0);
    puts("LEGACY_TIME_NORMALIZATION_OK raw_unchanged rollover leap invalid");
    return 0;
}
