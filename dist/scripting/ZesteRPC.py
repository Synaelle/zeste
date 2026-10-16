# Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
# Licensed under GPLv2 or any later version
# Refer to the license.txt file included.
#
# Zeste RPC client for agents and test scripts. Extends AzaharRPC (memory read/write,
# screenshots, perf stats) with the Zeste automation commands (core/zeste_automation.h):
#
#   rpc = ZesteRPC()
#   rpc.command("press A 5")
#   rpc.command("lcd both C:/tmp/shot.png")     # raw LCD contents -> shot_top.png / shot_bottom.png
#   rpc.command("dump-cmdlist C:/tmp/frame.txt")
#   rpc.command("home 120")
#   rpc.read_memory(0x14000000, 0x100)
#
# Start Zeste with --rpc (and usually --headless) so the RPC server listens on UDP 45987.
# Commands run at the next emulated VBlank; files appear once that frame has been emulated.

import struct

from AzaharRPC import AzaharRPC

ZESTE_COMMAND = 0x100


class ZesteRPC(AzaharRPC):
    def command(self, text):
        """Queues an automation command. Raises ValueError on a parse error."""
        data = text.encode("utf-8")
        reply = self._request(ZESTE_COMMAND, struct.pack("II", len(data), 0) + data)
        if len(reply) < 4:
            raise RuntimeError("Zeste command rejected by the server (old build or bad packet)")
        status = struct.unpack("I", reply[:4])[0]
        if status != 0:
            raise ValueError(reply[4:].decode("utf-8", "replace"))

    def report_bug(self, title, description=""):
        """Adds a BUG entry (with context and both screens) to Zeste's report list."""
        self.command("bug {} | {}".format(title, description.replace("\n", " ")))

    def report_inaccuracy(self, title, description=""):
        """Adds an INACCURACY entry (with context and both screens) to Zeste's report list."""
        self.command("inaccuracy {} | {}".format(title, description.replace("\n", " ")))


if __name__ == "__main__":
    import sys
    rpc = ZesteRPC(timeout=2)
    rpc.command(" ".join(sys.argv[1:]) if len(sys.argv) > 1 else "log hello from ZesteRPC")
    print("queued")
