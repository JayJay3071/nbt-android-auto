# Native NBT prototype
Installed: /var/opt/hmi/data/rw/NbtNativePrototype
Manual start after every reboot: sh /var/opt/hmi/data/rw/NbtNativePrototype/start.sh
Status: /fs/ram3/nbt-aa-native/status.txt
Open Multimedia / NBT Bildwechsel, then connect the unlocked OnePlus.

Pipeline: USB/TLS -> H264 FIFO -> loopback HTTP -> stock Cinemo software decoder -> YUV FIFO -> native PNG -> HMI image model. Input800x480, output480x288 up to3fps. No laptop decoder, no audio or controller input. No boot autostart. Persistent package is copied into RAM on manual start.

VM direct-file/FIFO conversion passed393 actual phone frames. Vehicle package and HMI readback verified, supervisor PID2842727 started waiting_for_phone, USB lease ready. Full HTTP/live vehicle operation is UNVERIFIED; user requested deployment without additional testing. New HMI library requires normal reboot.

Stop only the verified native_service supervisor PID with SIGTERM. It stops its own children and releases the USB lease. Never terminate stock io-nme or HmiMain3D. Inspect residual processes before clearing a stale lock. Three failed sessions require unplug/replug.

Previous HMI binary: build/native-prototype-20261106/plugin-before.so. Other plugins unchanged. Existing page test subtitle unchanged.

Vehicle update: Cinemo class-factory initialization initially failed because its configuration was not beside the executable. start.sh now copies the stock executable and both Cinemo XML configuration files into the RAM runtime; the service includes /opt/mm/lib/cinemo in its library path. HMI reported 14 native live updates with zero rejects, session1 running. User visual confirmation and long-term stability remain pending. See build/native-prototype-20261106/vehicle-first-native-live.txt. Persistent loose files are updated; the installation tar is an older snapshot and must not be re-extracted.

User visually confirmed: live Android Auto image is visible and updates on iDrive. This establishes native vehicle decoding and display for the current session. Long-term stability, audio, controller input and boot autostart remain unfinished. Service left running.
