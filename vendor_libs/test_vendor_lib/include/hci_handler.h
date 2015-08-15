//
// Copyright 2015 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "vendor_libs/test_vendor_lib/include/command_packet.h"
#include "vendor_libs/test_vendor_lib/include/hci_transport.h"
#include "vendor_libs/test_vendor_lib/include/packet.h"

namespace test_vendor_lib {

// Dispatches packets to the appropriate controller handler. These handlers
// must be registered by controller objects in order for commands to be
// processed. Unregistered commands will perform no operations. Exposes two
// callbacks, HandleCommand() and HandleData(), to be registered with a
// HciTransport object and called when commands and data are sent by the host.
class HciHandler {
 public:
  HciHandler() = default;

  ~HciHandler() = default;

  // Callback to be fired when a command packet is received from the HCI. Takes
  // ownership of the packet and dispatches work to the controller through the
  // callback registered with the command's opcode. After the controller
  // finishes processing the command and the callback returns, the command
  // packet is destroyed.
  void HandleCommand(std::unique_ptr<CommandPacket> command_packet);

  // Creates the mapping from the opcode to the method |callback|.
  // |callback|, which is provided by the controller, will be fired when its
  // command opcode is received from the HCI.
  void RegisterControllerCommand(
      std::uint16_t opcode,
      std::function<void(const std::vector<std::uint8_t> args)> callback);

  // Sets the command and data callbacks for when packets are received from the
  // HCI.
  void RegisterHandlersWithTransport(HciTransport& transport);

 private:
  // Controller callbacks to be executed in handlers and registered in
  // RegisterControllerCommand().
  std::unordered_map<std::uint16_t,
                     std::function<void(const std::vector<std::uint8_t> args)> >
      commands_;

  DISALLOW_COPY_AND_ASSIGN(HciHandler);
};

}  // namespace test_vendor_lib