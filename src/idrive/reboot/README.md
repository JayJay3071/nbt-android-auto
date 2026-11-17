# nbt_reset - "Restart system" on the Android Auto page

Multimedia -> **Android Auto** -> **Restart system** -> "Restart the head unit now?" -> **Restart now**
(pages 33 / 34 of `src/idrive/NbtAndroidAuto.xml`, code in `src/idrive/aa_entry_plugin.cpp`). Until 2026-11-12 this was
a separate plugin ("Restart iDrive" under Multimedia).

The button runs `nbt_reset`, which sends UDS `11 01` (hardReset) to the head unit (0x63) through the gateway's
HSFZ port 6801, the same request as Tool32's `STEUERGERAETE_RESET`. No HMI process is stopped.

Car 2026-11-12: `nbt_reset 169.254.102.32` on the head unit -> `reset accepted (51 01)`, the head unit rebooted.
The HSFZ broadcast does not reach the gateway from the head unit (`No route to host` for 169.254.255.255), so the
gateway address goes into the file `gateway` (one line) in `/var/opt/hmi/data/rw/NbtAndroidAutoRT/`. The installer
writes `nbt_reset` there but leaves `gateway` alone (it is car-specific).

Log: `/fs/ram3/nbt-reboot.log` (nbt_reset's output), the plugin's decisions in `/fs/ram3/nbt-aa-plugin.log`.
Test the tool without a reset: `nbt_reset -n <gateway>`. It needs `libsocket.so.3`, which a command run directly
through `ssh host "..."` does not find (no login profile); there, prefix the HMI's library path:
`LD_LIBRARY_PATH=/lib:/opt/sys/lib:/boot/lib:/opt/hmi/lib nbt_reset -n <gateway>`. The plugin passes the HMI's
environment.

## From a PC: `scripts/nbt_reset.py`

The same reset (UDS `11 01` to 0x63 through the gateway's HSFZ port 6801) from a laptop on the ENET cable, Python 3
standard library only:

    python scripts\nbt_reset.py                 find the gateway with the HSFZ 6811 broadcast, then reset
    python scripts\nbt_reset.py --host IP       skip discovery, use this gateway address
    python scripts\nbt_reset.py --type 2        1 hardReset (default), 2 keyOffOnReset, 3 softReset

From the PC the broadcast does reach the gateway, so the script prints the gateway address (and the VIN): that is the
address for the `gateway` file above.
