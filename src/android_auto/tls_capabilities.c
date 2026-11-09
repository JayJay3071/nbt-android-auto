/* Read the selected SSL library's version and exports; no network/TLS session. */
#include <dlfcn.h>
#include <stdio.h>

int main(int argc, char **argv) {
    void *lib;
    const char *path;
    const char *(*version)(int);
    int tls12;
    if (argc > 2) return 2;
    path = argc == 2 ? argv[1] : "libssl.so";
    lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        fprintf(stderr, "SSL_LOAD_FAILED %s: %s\n", path, dlerror());
        return 4;
    }
    version = (const char *(*)(int))dlsym(lib, "OpenSSL_version");
    if (!version) version = (const char *(*)(int))dlsym(lib, "SSLeay_version");
    tls12 = dlsym(lib, "TLSv1_2_client_method") != NULL;
    printf("SSL_LIBRARY=%s\nSSL_VERSION=%s\nTLS12_CLIENT_METHOD=%s\n",
           path, version ? version(0) : "unknown", tls12 ? "present" : "missing");
    dlclose(lib);
    /* This checks the reference receiver's required export, not a handshake. */
    return tls12 ? 0 : 3;
}
