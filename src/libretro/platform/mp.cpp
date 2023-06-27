/*
    Copyright 2023 Jesse Talavera-Greenberg

    melonDS DS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS DS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS DS. If not, see http://www.gnu.org/licenses/.
*/

#include <cstring>
#include <ctime>
#include <Platform.h>
#include <SPI.h>
#include "../environment.hpp"

#define GetUi32(p) (((const u8*)(p))[0] | ((u32)((const u8*)(p))[1] << 8) | ((u32)((const u8*)(p))[2] << 16) | ((u32)((const u8*)(p))[3] << 24))
#define GetUi64(p) (GetUi32(p) | ((u64)GetUi32(((const u8*)(p)) + 4) << 32))
#define SetUi32(p,v) { ((u8*)(p))[0] = (u8)((u32)(v) & 0xFF); ((u8*)(p))[1] = (u8)(((u32)(v) >> 8) & 0xFF); ((u8*)(p))[2] = (u8)(((u32)(v) >> 16) & 0xFF); ((u8*)(p))[3] = (u8)(((u32)(v) >> 24) & 0xFF); }
#define SetUi64(p,v) { SetUi32(p, v); SetUi32((u8*)(p)+4, (u64)(v) >> 32); }

namespace retro_mp {
    static bool _mp_on;
    static int _connections;
    static u16 _client_id;
    static std::vector<u8> _buf, _incoming;
    static retro_netpacket_send_t _send_fn;

    static bool BlockForNewIncoming();
    static int SendPacketGeneric(u32 type, u8* packet, int len, u64 timestamp);
    static int RecvPacketGeneric(u8* packet, bool block, u64* timestamp);
    static u16 RecvReplies(u8* packets, u64 timestamp, u16 aidmask);
    static void NetPacketStart(uint16_t client_id, retro_netpacket_send_t _send_fn);
    static void NetPacketReceive(const void* pkt, size_t pktlen, uint16_t client_id);
    static void NetPacketStop(void);
    static bool NetPacketConnected(uint16_t client_id);
    static void NetPacketDisconnected(uint16_t client_id);
    static void WriteFirmwareMacAddress();

    enum {
        MIN_PACKET_LEN = 4 + 8,
        MAX_PACKET_LEN = 4 + 8 + 2048,
        RECV_TIMEOUT = 250, // TODO: Make configurable (is 25 in upstream melonDS)
    };
}

bool retro_mp::BlockForNewIncoming() {
    // block until data arrives
    size_t start_incoming_size = _incoming.size();
    for (std::clock_t t_start = std::clock();;) {
        // check latest incoming
        if (_send_fn)
            _send_fn(0, 0, 0, 0, 0);
        if (_incoming.size() > start_incoming_size)
            return true;
        if (!_connections || ((std::clock() - t_start) * 1000 / CLOCKS_PER_SEC) > RECV_TIMEOUT)
            return false;
    }
}

int retro_mp::SendPacketGeneric(u32 type, u8* data, int len, u64 timestamp) {
    if (!_connections)
    {
        retro::log(RETRO_LOG_DEBUG, "[DSNET] SEND     - Discard packet of size %d due to no one connected\n", (int)(4 + 8 + (size_t)len));
        return len; // no one around to receive packets
    }

    size_t pktlen = 4 + 8 + (size_t)len;
    if (_buf.size() < pktlen)
        _buf.resize(pktlen);

    SetUi32(&_buf[0], type);
    SetUi64(&_buf[4], timestamp);
    std::memcpy(&_buf[4 + 8], data, (size_t)len);

    const u8* p = (pktlen >= (4 + 8 + 8) ? data : nullptr);
    retro::log(RETRO_LOG_DEBUG, "[DSNET] SEND     - Size: %5d - Type: %4x - Timestamp: %8d - Data: %02x%02x%02x%02x%02x%02x%02x%02x...\n",
        (int)pktlen, type, timestamp, (p ? p[0] : -1), (p ? p[1] : -1), (p ? p[2] : -1), (p ? p[3] : -1), (p ? p[4] : -1), (p ? p[5] : -1), (p ? p[6] : -1), (p ? p[7] : -1));

    // broadcast to everyone
    _send_fn(RETRO_NETPACKET_RELIABLE, &_buf[0], pktlen, 0xFFFF, true);
    return len;
}

int retro_mp::RecvPacketGeneric(u8* out_data, bool block, u64* out_timestamp) {
    // if no other player exists: return early
    if (!_connections)
    {
        retro::log(RETRO_LOG_DEBUG, "[DSNET] RECV GEN - Block: %d - No one connected, cannot receive\n", (int)block);
        return 0;
    }

    for (;;) {
        for (size_t i = 0, pktlen; i < _incoming.size(); i += 8 + pktlen) {
            pktlen = (size_t)GetUi64(&_incoming[i + 0]);
            u32 type = GetUi32(&_incoming[i + 8]);
            if ((type & 0xFFFF) == 2)
                continue; // ignore replies

            u64 timestamp = GetUi64(&_incoming[i + 8 + 4]);
            int len = (int)(pktlen - (8 + 4));

            const u8* p = (pktlen >= (4 + 8 + 8) ? &_incoming[i + 8 + 4 + 8] : nullptr);
            retro::log(RETRO_LOG_DEBUG, "[DSNET] RECV GEN - Size: %5d - Type: %4x - Timestamp: %8d - Data: %02x%02x%02x%02x%02x%02x%02x%02x...\n",
                (int)pktlen, type, timestamp, (p ? p[0] : -1), (p ? p[1] : -1), (p ? p[2] : -1), (p ? p[3] : -1), (p ? p[4] : -1), (p ? p[5] : -1), (p ? p[6] : -1), (p ? p[7] : -1));

            if (out_data)
                std::memcpy(out_data, &_incoming[i + 8 + 4 + 8], len);

            if (out_timestamp)
                *out_timestamp = timestamp;

            // consume from incoming buffer
            _incoming.erase(_incoming.begin() + i, _incoming.begin() + i + 8 + pktlen);

            return len;
        }

        // wait for another packet
        if (!block || !BlockForNewIncoming())
        {
            retro::log(RETRO_LOG_DEBUG, "[DSNET] RECV GEN - Block: %d - No incoming packet, cannot receive\n", (int)block);
            return 0;
        }
    }
}

u16 retro_mp::RecvReplies(u8* packets, u64 timestamp, u16 aidmask) {
    // if no other player exists: return early
    if (!_connections)
    {
        retro::log(RETRO_LOG_DEBUG, "[DSNET] RECV REP - AidMask: %04x - No one connected, cannot receive\n", aidmask);
        return 0;
    }

    for (u16 ret = 0;;) {
        for (size_t i = 0, pktlen; i < _incoming.size(); i += 8 + pktlen) {
            pktlen = (size_t)GetUi64(&_incoming[i + 0]);
            u32 type = GetUi32(&_incoming[i + 8]);
            if ((type & 0xFFFF) != 2)
                continue; // not a reply

            u64 pkt_timestamp = GetUi64(&_incoming[i + 8 + 4]);
            if (pkt_timestamp >= (timestamp - 32)) { // relevant packet
                int len = (int)(pktlen - (8 + 4));
                u32 aid = (type >> 16);
                if (packets && aid && len <= 1024)
                    std::memcpy(&packets[(aid-1)*1024], &_incoming[i + 8 + 4 + 8], len);
                ret |= (1 << aid);
            }

            const u8* p = (pktlen >= (4 + 8 + 8) ? &_incoming[i + 8 + 4 + 8] : nullptr);
            retro::log(RETRO_LOG_DEBUG, "[DSNET] RECV REP - Size: %5d - Type: %4x - Timestamp: %8d - Data: %02x%02x%02x%02x%02x%02x%02x%02x...\n",
                (int)pktlen, type, pkt_timestamp, (p ? p[0] : -1), (p ? p[1] : -1), (p ? p[2] : -1), (p ? p[3] : -1), (p ? p[4] : -1), (p ? p[5] : -1), (p ? p[6] : -1), (p ? p[7] : -1));

            // consume from incoming buffer
            _incoming.erase(_incoming.begin() + i, _incoming.begin() + i + 8 + pktlen);
        }

        if ((ret & aidmask) == aidmask) {
            // all the clients have sent their reply
            retro::log(RETRO_LOG_DEBUG, "[DSNET] RECV REP - AidMask: %04x - Ret: %04x - Got all replies\n", aidmask, ret);
            return ret;
        }

        // wait for another packet
        if (!BlockForNewIncoming()) {
            retro::log(RETRO_LOG_DEBUG, "[DSNET] RECV REP - AidMask: %04x - Ret: %04x - No incoming packet, cannot receive all\n", aidmask, ret);
            return ret; // no more replies available
        }
    }
}

void retro_mp::WriteFirmwareMacAddress() {
    // store client_id in firmware mac address
    u8* mac = SPI_Firmware::GetWifiMAC();
    mac[4] = (u8)(_client_id >> 8);
    mac[5] = (u8)(_client_id & 255);
    retro::log(RETRO_LOG_DEBUG, "[DSNET] Write my client id %d to Firmware Mac address: %02x:%02x:%02x:%02x:%02x:%02x\n",
        _client_id, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void retro_mp::NetPacketStart(uint16_t client_id, retro_netpacket_send_t send_fn) {
    retro::log(RETRO_LOG_DEBUG, "[DSNET] NetPacketStart - My client id: %d\n", client_id);
    _send_fn = send_fn;
    _client_id = client_id;
    WriteFirmwareMacAddress();
    if (client_id != 0) {
        // I am a client already connected to the host
        _connections = 1;
    }
}

void retro_mp::NetPacketReceive(const void* pkt, size_t pktlen, uint16_t client_id) {
    if (!_mp_on)
    {
        retro::log(RETRO_LOG_DEBUG, "[DSNET] INCOMING - Discard packet of size %d from client %d while mp was off\n", (int)pktlen, client_id);
        return; // mp hasn't begun yet
    }

    if (pktlen < MIN_PACKET_LEN || pktlen > MAX_PACKET_LEN)
    {
        retro::log(RETRO_LOG_DEBUG, "[DSNET] INCOMING - Discard packet of size %d from client %d due to invalid length\n", (int)pktlen, client_id);
        return; // invalid length
    }


    u32 type = GetUi32(&((const u8*)pkt)[0]);
    u64 timestamp = GetUi64(&((const u8*)pkt)[4]);
    const u8* p = (pktlen >= (4 + 8 + 8) ? &((const u8*)pkt)[4 + 8] : nullptr);
    retro::log(RETRO_LOG_DEBUG, "[DSNET] INCOMING - Size: %5d - Type: %4x - Timestamp: %8d - Data: %02x%02x%02x%02x%02x%02x%02x%02x... - Source: %d\n",
        (int)pktlen, type, timestamp, (p ? p[0] : -1), (p ? p[1] : -1), (p ? p[2] : -1), (p ? p[3] : -1), (p ? p[4] : -1), (p ? p[5] : -1), (p ? p[6] : -1), (p ? p[7] : -1), client_id);

    size_t incoming_ofs = _incoming.size(), total_len = 8 + pktlen;
    _incoming.resize(incoming_ofs + total_len);
    SetUi64(&_incoming[incoming_ofs], pktlen);
    std::memcpy(&_incoming[incoming_ofs + 8], pkt, pktlen);
}

void retro_mp::NetPacketStop(void) {
    retro::log(RETRO_LOG_DEBUG, "[DSNET] NetPacketStop\n");
    _connections = 0;
    _incoming.clear();
    _send_fn = nullptr;
}

static bool retro_mp::NetPacketConnected(uint16_t client_id) {
    _connections++;
    retro::log(RETRO_LOG_DEBUG, "[DSNET] Connected - Client ID: %d - Connections: %d\n", client_id, _connections);
    return true;
}

static void retro_mp::NetPacketDisconnected(uint16_t client_id) {
    _connections--;
    retro::log(RETRO_LOG_DEBUG, "[DSNET] Disconnected - Client ID: %d - Connections: %d\n", client_id, _connections);
}

bool Platform::MP_Init() {
    retro::log(RETRO_LOG_DEBUG, "[DSNET] MP_Init\n");
    static const retro_netpacket_callback packet_callbacks = {
        retro_mp::NetPacketStart,
        retro_mp::NetPacketReceive,
        retro_mp::NetPacketStop,
        nullptr, // poll
        retro_mp::NetPacketConnected,
        retro_mp::NetPacketDisconnected
    };
    retro::environment(RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE, (void *)&packet_callbacks);
    return true;
}

void Platform::MP_DeInit() {
    retro::log(RETRO_LOG_DEBUG, "[DSNET] MP_DeInit\n");
    retro_mp::_incoming.clear();
}

void Platform::MP_Begin() {
    retro::log(RETRO_LOG_DEBUG, "[DSNET] MP_Begin\n");
    retro_mp::_mp_on = true;
    retro_mp::WriteFirmwareMacAddress();
}

void Platform::MP_End() {
    retro::log(RETRO_LOG_DEBUG, "[DSNET] MP_End\n");
    retro_mp::_mp_on = false;
}

int Platform::MP_SendPacket(u8 *data, int len, u64 timestamp) {
    return retro_mp::SendPacketGeneric(0, data, len, timestamp);
}

int Platform::MP_RecvPacket(u8 *data, u64 *timestamp) {
    return retro_mp::RecvPacketGeneric(data, false, timestamp);
}

int Platform::MP_SendCmd(u8 *data, int len, u64 timestamp) {
    return retro_mp::SendPacketGeneric(1, data, len, timestamp);
}

int Platform::MP_SendReply(u8 *data, int len, u64 timestamp, u16 aid) {
    return retro_mp::SendPacketGeneric(2 | (aid<<16), data, len, timestamp);
}

int Platform::MP_SendAck(u8 *data, int len, u64 timestamp) {
    return retro_mp::SendPacketGeneric(3, data, len, timestamp);
}

int Platform::MP_RecvHostPacket(u8 *data, u64 *timestamp) {
    return retro_mp::RecvPacketGeneric(data, true, timestamp);
}

u16 Platform::MP_RecvReplies(u8 *data, u64 timestamp, u16 aidmask) {
    return retro_mp::RecvReplies(data, timestamp, aidmask);
}
