#pragma once

#include <cstdint>

namespace cookielink {
namespace relay {

static constexpr uint16_t kProtocolVersion = 1;
static constexpr uint32_t kMaxFrameSize = 1024 * 1024;

enum MessageType : uint8_t {
    MsgHello = 1,
    MsgWelcome = 2,
    MsgJoinGroup = 3,
    MsgJoinResult = 4,
    MsgPeerJoin = 5,
    MsgPeerLeave = 6,
    MsgData = 7,
    MsgError = 8,
    MsgLeaveGroup = 9,
    MsgPublicGroupsRequest = 10,
    MsgPublicGroupsUpdate = 11
};

} // namespace relay
} // namespace cookielink
