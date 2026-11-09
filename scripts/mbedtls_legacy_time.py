"""Checked, private-library overlay for legacy AA UTCTime numeric offsets.

RFC 5280 requires Z; old AA identities use ASN.1 numeric offsets. Preserve all
signed bytes and normalize the parsed date only. No expiry/signature checks are
disabled. This is an explicit compatibility extension, not a system TLS patch.
"""


def patch_x509(source):
    anchor = "    /* len is 12 or 14 depending on year_len, plus optional trailing 'Z' */"
    if source.count(anchor) != 1:
        raise ValueError('Mbed TLS x509 time parser changed')
    extension = r'''
    /* NBT compatibility: legacy UTCTime YYMMDDHHMMSS+/-hhmm.
     * Normalize parsed fields only; raw certificate/TBS bytes stay untouched.
     * Scope deliberately excludes GeneralizedTime and missing seconds. */
    if (year_len == 2 && len == 17) {
        const unsigned char *s = *p;
        int hours, minutes, offset, day_minutes, days;
        static const int month_days[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
        size_t i;
        if (s[12] != '+' && s[12] != '-') return MBEDTLS_ERR_X509_INVALID_DATE;
        for (i = 13; i < 17; ++i)
            if (s[i] < '0' || s[i] > '9') return MBEDTLS_ERR_X509_INVALID_DATE;
        hours = (s[13]-'0')*10 + s[14]-'0';
        minutes = (s[15]-'0')*10 + s[16]-'0';
        if (hours > 23 || minutes > 59) return MBEDTLS_ERR_X509_INVALID_DATE;
        ret = x509_parse_time(s, tm, 2);
        if (ret != 0) return ret;
        offset = hours*60 + minutes;
        day_minutes = tm->hour*60 + tm->min + (s[12] == '-' ? offset : -offset);
        if (day_minutes < 0) {
            day_minutes += 1440;
            if (--tm->day == 0) {
                if (--tm->mon == 0) { tm->mon = 12; --tm->year; }
                tm->day = month_days[tm->mon];
                if (tm->mon == 2 && tm->year % 4 == 0 &&
                    (tm->year % 100 != 0 || tm->year % 400 == 0)) ++tm->day;
            }
        } else if (day_minutes >= 1440) {
            day_minutes -= 1440;
            days = month_days[tm->mon];
            if (tm->mon == 2 && tm->year % 4 == 0 &&
                (tm->year % 100 != 0 || tm->year % 400 == 0)) ++days;
            if (++tm->day > days) {
                tm->day = 1;
                if (++tm->mon == 13) { tm->mon = 1; ++tm->year; }
            }
        }
        tm->hour = day_minutes / 60; tm->min = day_minutes % 60;
        *p += len;
        return 0;
    }

'''
    return source.replace(anchor, extension + anchor)
