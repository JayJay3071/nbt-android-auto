/* Isolated loader check: never calls the plugin factory or any HMI service. */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct plugin_info {
    uint16_t major, minor;
    const char *name, *vendor;
    void *create, *destroy;
};

int main(int argc, char **argv)
{
    void *handle;
    struct plugin_info *info;
    if ((argc != 2 && argc != 3) || sizeof(struct plugin_info) != 20) return 2;
    handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!handle) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 3; }
    info = (struct plugin_info *)dlsym(handle, "pluginAppInfo");
    if (!info || info->major != 1 || info->minor != 0 ||
        !info->name || strcmp(info->name, argc == 3 ? argv[2] : "HelloNbt") ||
        !info->vendor || strcmp(info->vendor, "NBT-Project") ||
        !info->create || !info->destroy) {
        fputs("Plugin metadata mismatch\n", stderr);
        dlclose(handle);
        return 4;
    }
    puts("NBT_PLUGIN_LINKAGE_OK");
    puts("Factory was NOT called; menu registration remains untested.");
    return dlclose(handle) == 0 ? 0 : 5;
}
