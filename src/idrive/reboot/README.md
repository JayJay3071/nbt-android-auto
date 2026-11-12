# NbtReboot - "Restart iDrive" entry

A separate HMI plugin under Multimedia: **Restart iDrive** -> "Restart the head unit now?" -> **Restart now**.
The button runs `nbt_reset`, which sends UDS `11 01` (hardReset) to the head unit (0x63) through the gateway's
HSFZ port 6801, the same request as Tool32's `STEUERGERAETE_RESET`. No HMI process is stopped.

Car 2026-11-12: `nbt_reset 169.254.102.32` on the head unit -> `reset accepted (51 01)`, the head unit rebooted.
The HSFZ broadcast does not reach the gateway from the head unit (`No route to host` for 169.254.255.255), so the
gateway address goes into the file `gateway` in the plugin folder.

Folder `/var/opt/hmi/data/rw/Plugins/NbtReboot/`:

| File | From | Mode |
|---|---|---|
| `nbt_reset` | `build/.../programs/` | 700 |
| `libNbtReboot.so` | `build/.../programs/` | 755 |
| `NbtReboot.xml` | `build/.../reboot-resources/` | 644 |
| `NbtReboot_ALL_Texts.zip`, `NbtReboot_ALL_Images.zip` | `build/.../reboot-resources/` | 644 |
| `gateway` | one line, the gateway address | 644 |
| `NbtReboot.manifest` | `build/.../reboot-resources/` - write it last | 644 |

The entry appears after the next HMI start. Remove: delete the folder. Log: `/fs/ram3/nbt-reboot.log`.
Test the tool without a reset: `nbt_reset -n <gateway>`.
