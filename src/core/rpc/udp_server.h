// Copyright 2019 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#pragma once

#include <functional>
#include <memory>
#include "common/common_types.h"

namespace Core::RPC {

class Packet;

/// Port the RPC server binds next (45987 by default, 0 = a free port picked by the system).
void SetUdpPort(u16 port);
/// Port the running RPC server is bound to (0 when none).
u16 BoundUdpPort();

class UDPServer {
public:
    explicit UDPServer(std::function<void(std::unique_ptr<Packet>)> new_request_callback);
    ~UDPServer();

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Core::RPC
