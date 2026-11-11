/* Original NBT x86 static RHMI page prototype. Not yet vehicle validated.
 * ABI declarations are based on this vehicle's exported symbols and its
 * 20-byte SecurityMenu pluginAppInfo. No Evo offsets or vtable hooks.
 */
#include <stdint.h>
#include <new>

#ifndef NBT_PLUGIN_NAME
#define NBT_PLUGIN_NAME "HelloNbt"
#endif

#if !defined(__QNXNTO__) || !defined(__i386__)
#error This prototype requires QNX Neutrino 32-bit x86.
#endif

namespace bmw {
class IDispatcher;
class Bundle;
class IPluginApp;
template <typename T> class BasicString {
public:
    BasicString(const T*);
    BasicString(const BasicString&);
    ~BasicString();
private:
    T* data;
};
class PluginApp {
public:
    PluginApp(IDispatcher&, BasicString<char>, const Bundle&, const Bundle&);
    ~PluginApp();
private:
    /* Opaque allocation reserve, not a declaration of the internal layout.
     * Verify the constructor's highest written offset before deployment. */
    void* reserved[1024];
};
}

static bmw::IPluginApp* createHello(bmw::IDispatcher& dispatcher,
    const char* name, const bmw::Bundle& parameters, const bmw::Bundle& config)
{
    bmw::PluginApp* app = new bmw::PluginApp(
        dispatcher, bmw::BasicString<char>(name), parameters, config);
    return reinterpret_cast<bmw::IPluginApp*>(app);
}

static void destroyHello(bmw::IPluginApp* app)
{
    delete reinterpret_cast<bmw::PluginApp*>(app);
}

struct PluginAppInfo {
    uint16_t major;
    uint16_t minor;
    const char* name;
    const char* vendor;
    bmw::IPluginApp* (*create)(bmw::IDispatcher&, const char*,
        const bmw::Bundle&, const bmw::Bundle&);
    void (*destroy)(bmw::IPluginApp*);
};
typedef char RequireObservedInfoSize[(sizeof(PluginAppInfo) == 20) ? 1 : -1];

extern "C" {
PluginAppInfo pluginAppInfo = {
    1, 0, NBT_PLUGIN_NAME, "NBT-Project", createHello, destroyHello
};
}
