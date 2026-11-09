#ifndef NBT_TLS_USB_PROBE_H
#define NBT_TLS_USB_PROBE_H
#ifdef __cplusplus
extern "C" {
#endif
/* Input: capacity in *size, actual bytes out. Output: exact bytes in *size. */
typedef int (*nbt_tls_transfer)(void*, int input, unsigned char*, unsigned* size);
int nbt_tls_probe_init(void);
int nbt_tls_probe_run(nbt_tls_transfer transfer, void* context, int discovery);
void nbt_tls_probe_free(void);
#ifdef __cplusplus
}
#endif
#endif
