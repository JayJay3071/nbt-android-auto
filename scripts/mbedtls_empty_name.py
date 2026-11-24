# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Valentin Radu (NOTICE.md, "GPLv3 parts")
"""Mbed TLS overlay for the CarPlay work: accept an empty X.509 Name.

The lockdown pair record (as libimobiledevice and xcertplay make it) has certificates with an empty issuer and subject
(an empty SEQUENCE). Mbed TLS rejects those when parsing ("name tag or value is invalid: out of data"), which it must
do for its own TLS identity (the root certificate) and for the iPhone's certificate (the DeviceCertificate we made).
An empty Name is valid DER; it is parsed as no name. Nothing else changes.
"""


def patch_x509_empty_name(source):
    anchor = """    /* don't use recursion, we'd risk stack overflow if not optimized */
    while (1) {
        /*
         * parse SET
         */"""
    if source.count(anchor) != 1:
        raise ValueError('Mbed TLS x509 name parser changed')
    return source.replace(anchor, """    /* NBT (CarPlay pair record): an empty Name is no name */
    if (*p == end) {
        return 0;
    }

""" + anchor)
