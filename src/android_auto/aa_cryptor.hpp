// Alternate backend for the reference AASession's AACryptor interface.
// Interface adapted from NBT-AAD, Copyright (c) 2026 nanaisu.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 nanaisu - NBT-AAD (https://github.com/Sq00ky/Supra-iDrive6-Android-Auto)
#ifndef AA_CRYPTOR_HPP
#define AA_CRYPTOR_HPP
#include <stdint.h>
#include <stddef.h>
#include <vector>

namespace nbt {
class AACryptor {
public:
    AACryptor();
    ~AACryptor();
    bool init();
    void cleanup();
    bool startHandshake(std::vector<uint8_t>& out);
    bool processHandshakeData(const uint8_t* data, size_t n, std::vector<uint8_t>& out);
    bool isHandshakeComplete() const;
    bool encrypt(const uint8_t* data, size_t n, std::vector<uint8_t>& out);
    bool decrypt(const uint8_t* data, size_t n, std::vector<uint8_t>& out);
    const char* lastError() const { return error_; }
    size_t pendingOut() const;
private:
    struct State;
    State* state_;
    char error_[192];
    bool failure(const char* operation, int code);
    bool feed(const uint8_t* data, size_t n);
    bool handshake(std::vector<uint8_t>& out);
    AACryptor(const AACryptor&);
    AACryptor& operator=(const AACryptor&);
};
}
#endif
