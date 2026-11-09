/* Console-only smoke test. No devices, display APIs or services are accessed. */
#include <stdio.h>

#if !defined(__QNXNTO__) || !defined(__i386__)
#error Build this program for QNX Neutrino 32-bit x86.
#endif

int main(void)
{
    if (puts("Hello World from QNX x86!") == EOF)
        return 1;
    return fflush(stdout) == 0 ? 0 : 1;
}
