// NBT-Project (2026-11-15): requests to the original HU_NBT's audio ConnectionMaster, FBlock 0x03, as HmiMain3D sends
// them (offline analysis, docs/AA-AUDIO-PLAN.md "Offline-Analyse"). Header only, so tests/test_connmaster.cpp checks the
// exact bytes nbt_audioconn sends.
#ifndef MOST_CONNMASTER_HPP
#define MOST_CONNMASTER_HPP

#include "most/msg.hpp"
#include <stdint.h>

namespace nbt {
namespace most {
namespace connmaster {

const uint8_t  kFBlock     = 0x03;
const uint16_t kFktBuild   = 0x053;   // BuildHighSyncConnection  (HmiMain3D request builder 0x8b832e6)
const uint16_t kFktRemove  = 0x054;   // RemoveHighSyncConnection (0x8b83258)
const uint16_t kFktTable   = 0xC03;   // HighSyncConnectionTable  (property)
const uint8_t  kOpStartResultAck = 0x6, kOpGet = 0x1, kOpResultAck = 0xD, kOpErrorAck = 0x9, kOpError = 0xF;

// The browser audio connections (EAudioSource 0x53/0x54/0x55 -> ConnID, table 0x9879224): front, rear 1, rear 2.
const uint16_t kConnBrowserFront = 0xB8;
inline bool isBrowserConn(unsigned long c) { return c == 0xB8 || c == 0xB9 || c == 0xBA; }

// Build (build=true) or remove a connection: StartResultAck, payload big-endian { u16 senderHandle, u16 connId }.
// dst / inst come from the DSYSMOSTServer registry; src 0 and hdrTag 0xFFFF are filled in by Client::send.
inline Msg connectionRequest(bool build, uint16_t dst, uint8_t inst, uint16_t handle, uint16_t conn) {
    Msg m;
    m.txType = 0; m.varTag = 1; m.varVal = 0;   // eTransmissionTXControl, as nbt_hwvideo sends (vehicle-tested)
    m.dst = dst; m.src = 0;
    m.fblock = kFBlock; m.inst = inst;
    m.fkt = build ? kFktBuild : kFktRemove;
    m.opType = kOpStartResultAck;
    m.hdrTag = 0xFFFFu;
    m.data.push_back((uint8_t)(handle >> 8));
    m.data.push_back((uint8_t)handle);
    m.data.push_back((uint8_t)(conn >> 8));
    m.data.push_back((uint8_t)conn);
    return m;
}

// Get of the connection table (read-only).
inline Msg tableGet(uint16_t dst, uint8_t inst) {
    Msg m;
    m.txType = 0; m.varTag = 1; m.varVal = 0;
    m.dst = dst; m.src = 0;
    m.fblock = kFBlock; m.inst = inst;
    m.fkt = kFktTable; m.opType = kOpGet; m.hdrTag = 0xFFFFu;
    return m;
}

// ResultAck payload: u16 senderHandle, big-endian. -1 when too short.
inline int resultAckHandle(const uint8_t* d, size_t n) { return n >= 2 ? (int)(d[0] << 8 | d[1]) : -1; }

} // namespace connmaster
} // namespace most
} // namespace nbt

#endif
