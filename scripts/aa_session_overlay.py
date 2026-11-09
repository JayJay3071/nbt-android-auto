"""Checked transformations of the pinned upstream session; never edit its checkout.

These changes are for our Mbed TLS port. Fail closed if upstream text changes.
Derived from NBT-AAD, Copyright (c) 2026 nanaisu.
SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
Required Notice: Copyright (c) 2026 nanaisu - NBT-AAD (https://github.com/Sq00ky/Supra-iDrive6-Android-Auto)
"""


def harden_session(source):
    def replace(old, new):
        nonlocal source
        if source.count(old) != 1:
            raise ValueError("Session overlay anchor changed: " + old[:90])
        source = source.replace(old, new)

    replace('void AASession::sessionLoop() {', '''void AASession::sessionLoop() {
    // The session thread owns TLS. A reconnect must not inherit old keys/state.
    cryptor_.cleanup();
    enum Phase { AwaitVersion, Handshaking, Ready };
    Phase phase = AwaitVersion;''')
    replace('        noteRx(channel, flags, length);', '''        if (channel >= 32 || length == 0 || ((flags & 0x08) && phase != Ready)) {
            printf("[AASession] Invalid channel, empty frame or premature encryption.\\n");
            break;
        }
        noteRx(channel, flags, length);''')
    start = source.index('                    printf("[AASession] Failed to decrypt encrypted frame')
    end = source.index('\n                }', start)
    replace(source[start:end], '''                    // WANT_READ is not a TLS failure. Preserve any AA fragments.
                    if (cryptor_.isHandshakeComplete()) continue;
                    printf("[AASession] Terminal TLS read error: %s\\n", cryptor_.lastError());
                    break;''')
    replace('            noteRxMsg(msg_id);', '''            const bool version = channel == 0 && msg_id == 2;
            const bool tls = channel == 0 && msg_id == 3;
            const bool ping = channel == 0 && msg_id == 11;
            if (frame_data.size() < 2 ||
                (version && (phase != AwaitVersion || (flags & 0x08))) ||
                (tls && (phase != Handshaking || (flags & 0x08))) ||
                (!version && !tls && !ping && (phase != Ready || !(flags & 0x08)))) {
                printf("[AASession] Message rejected in current authentication phase.\\n");
                break;
            }
            noteRxMsg(msg_id);''')
    replace('                if (msg_id == 2) { // VersionResponse', '''                if (msg_id == 2) { // VersionResponse
                    if (frame_data.size() < 8) break;''')
    start = source.index('                        // Status 0 =')
    end = source.index('                        printf("[Feature] aa_version:', start)
    replace(source[start:end], '''                        if (status != 0 || major != 1) {
                            printf("[AASession] Protocol refused: %u.%u status=%u\\n", major, minor, status);
                            break;
                        }
''')
    start = source.index('                        // Step 2: Initialize Cryptor')
    end = source.index('\n                    }\n                } else if (msg_id == 3)', start)
    replace(source[start:end], '''                        std::vector<uint8_t> tls_client_hello;
                        if (!cryptor_.init() || !cryptor_.startHandshake(tls_client_hello) ||
                            !sendSslHandshake(tls_client_hello)) {
                            printf("[AASession] TLS initialization failed: %s\\n", cryptor_.lastError());
                            break;
                        }
                        phase = Handshaking;''')
    replace('''                        printf("[AASession] Ignoring empty SSL handshake message.\\n");
                        continue;''', '''                        printf("[AASession] Empty SSL handshake message.\\n");
                        break;''')
    replace('                    cryptor_.processHandshakeData(&frame_data[2], frame_data.size() - 2, tls_out);', '''                    if (!cryptor_.processHandshakeData(&frame_data[2], frame_data.size() - 2, tls_out)) {
                        printf("[AASession] TLS handshake failed: %s\\n", cryptor_.lastError());
                        break;
                    }''')
    replace('                        sendSslHandshake(tls_out);', '                        if (!sendSslHandshake(tls_out)) break;')
    replace('                        sendAuthComplete();', '''                        if (!sendAuthComplete()) break;
                        phase = Ready;''')
    replace('                    sendServiceDiscoveryResponse();', '                    if (!sendServiceDiscoveryResponse()) break;')
    replace('    printf("[AASession] Session loop terminated.\\n");', '''    cryptor_.cleanup();
    printf("[AASession] Session loop terminated.\\n");''')
    return source
