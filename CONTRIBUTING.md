# Contributing

Thanks for wanting to help! This project started as one person's experiment on one car. It gets better with every
car, phone and pair of hands. Issues and pull requests are welcome in **English or German**.

Questions or suggestions that do not fit an issue: **androidauto.nbt@proton.me** (also PayPal for donations).

## Ways to help

- **Test on your car** and report the result, good or bad: car model, build year, NBT software level (I-Stufe),
  phone, and the diagnostic ZIP from the installer. "Works on my F30 from 2015" is valuable information too.
- **Fix bugs or add features** (see below).
- **Improve the documentation**: setup steps that were unclear to you are worth a pull request.

## Where help is needed

1. **Android Auto audio.** Today music goes over Bluetooth. The plan and current findings are in
   [docs/AA-AUDIO-PLAN.md](docs/AA-AUDIO-PLAN.md) (audio source switching over MOST, like the video path).
2. **Reproducible build.** One script that builds every QNX binary and the plugin from a clean checkout plus the QNX
   SDP, see [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).
3. **Other cars and NBT software levels**, and possibly NBT Evo (different DSI library).
4. **Latency**: the Cinemo HTTP buffer adds controller delay. Hardware decoding (`-wd 1`) was tried and made it worse
   (larger io-nme queue), see the comment in `src/android_auto/nbt_decode_runner.c`.
5. **Signed release builds** (Windows code signing, macOS notarization).
6. **Lane guidance** in the cluster has not been seen with a real phone yet.

## Before you start

- To build anything for the head unit you need **QNX 6.5.0** in a VM. You can download it from QNX; the setup (video
  walkthrough and a BIOS date tip) is in [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md#setting-up-qnx-650).

- Read the safety rules in [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md#safety-rules-learned-the-hard-way). Some
  experiments restart the head unit; a few rules keep the car usable.
- For larger changes, open an issue first so we can agree on the approach.

## Pull requests

- Keep each pull request focused on one change and describe **how you tested it** (VM, car model, phone).
- Say clearly what was verified on a real car and what was not. "Builds in the VM, not tested on a car" is fine.
- Code style: follow the surrounding code. C/C++ for QNX 6.5 (gcc 4.4, `-std=gnu++0x`, no exceptions/RTTI in the
  plugin), Python 3 for tools, PowerShell 5.1 compatible scripts. Shell scripts for the car use plain `sh` with LF line
  endings.
- By contributing you agree that your contribution is licensed under the project's
  [PolyForm Noncommercial License 1.0.0](LICENSE).

## Never commit

- Passwords, private SSH keys, `known_hosts` files of your car.
- Libraries, binaries or firmware dumps from the head unit (BMW, Harman, NVIDIA, Cinemo) or the QNX SDP.
- Logs or screenshots that show your VIN, phone numbers, contacts or locations. Check diagnostic ZIPs before you attach
  them publicly.

`.local/` and `build/` are ignored by git for exactly these files.
