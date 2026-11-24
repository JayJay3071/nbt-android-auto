/* nbt_driver_stub: stands in for the stock driver program io-fs-media when the USB launcher starts it through the
 * link accessory_window sets (iPod trial, 2026-11-14).
 *   started as the iPod driver ("-dipod,..."): hands over to /fs/ram3/aatool/ipod-start.sh, which mounts our iPod
 *     stand-in at the path the launcher waits for; the launcher then announces the device to the HMI.
 *   started as anything else (e.g. "-dpfs,..." for a phone in file-transfer mode): ends at once with an error, so the
 *     launcher gives the device up quickly, as it did while the path was blocked.
 * Every start is noted in /fs/ram3/aatool/stub.log. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char** argv)
{
    FILE* f = fopen("/fs/ram3/aatool/stub.log", "a");
    int i;
    if (f) {
        fprintf(f, "%ld started pid %d:", (long)time(0), (int)getpid());
        for (i = 0; i < argc; ++i)
            fprintf(f, " [%s]", argv[i]);
        fputc('\n', f);
        fclose(f);
    }
    if (argc >= 2 && !strncmp(argv[1], "-dipod", 6)) {
        execl("/bin/sh", "sh", "/fs/ram3/aatool/ipod-start.sh", argv[1], (char*)0);
        return 3;
    }
    return 1;
}
