# Head unit safety

**NEVER run `/sbin/shutdown` on the BMW NBT head unit.** This applies to humans and agents, whether working over
SSH, on the serial console, or through scripts or automation. The command causes a boot lock that can only be cleared
by disconnecting the vehicle battery.

To reset the head unit, **use the vehicle's ZGW (central gateway)** to send the diagnostic reset to the NBT.
Follow the [documented NBT reset procedure](src/idrive/reboot/README.md).
