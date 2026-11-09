/* Reuse the already vehicle-tested base PluginApp ABI, with a separate name.
 * Rendering is entirely through static HMI image resources; no graphics calls.
 */
#define NBT_PLUGIN_NAME "NbtVideoTest"
#include "hello_plugin.cpp"
