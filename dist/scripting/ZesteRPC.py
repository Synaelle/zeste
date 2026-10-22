# Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
# Licensed under GPLv2 or any later version
# Refer to the license.txt file included.
#
# Zeste RPC client for test scripts. Extends AzaharRPC (memory read/write,
# screenshots, perf stats) with the Zeste automation commands (core/zeste_automation.h):
#
#   rpc = ZesteRPC()
#   rpc.command("press A 5")
#   rpc.command("lcd both C:/tmp/shot.png")     # raw LCD contents -> shot_top.png / shot_bottom.png
#   rpc.command("dump-cmdlist C:/tmp/frame.txt")
#   rpc.command("home 120")
#   rpc.read_memory(0x14000000, 0x100)
#
# Start Zeste with --rpc (and usually --headless) so the RPC server listens on UDP 45987, or
# --rpc-port N (0 = a free port, printed as "Zeste RPC port: N"): ZesteRPC(port=N).
# Screenshots of the presented frame: screenshot() (AzaharRPC, no files involved).
# Commands run at the next emulated VBlank; files appear once that frame has been emulated.

import json
import struct

from AzaharRPC import AzaharRPC

ZESTE_COMMAND = 0x100
ZESTE_STATE = 0x101
ZESTE_OUTPUT = 0x102


class ZesteRPC(AzaharRPC):
    def command(self, text, wait=False):
        """Queues an automation command. Raises ValueError on a parse error.

        With wait=True the call returns once the command has run, with its result ("ok", or
        what went wrong), and raises TimeoutError if it has not run within 15 s (emulation
        paused or stopped). Arguments with spaces go in double quotes."""
        data = text.encode("utf-8")
        reply = self._request(ZESTE_COMMAND, struct.pack("II", len(data), 1 if wait else 0) + data,
                              timeout=20 if wait else None)
        if len(reply) < 4:
            raise RuntimeError("Zeste command rejected by the server (old build or bad packet)")
        status = struct.unpack("I", reply[:4])[0]
        message = reply[4:].decode("utf-8", "replace")
        if status == 1:
            raise ValueError(message)
        if status == 2:
            raise TimeoutError(message)
        return message if wait else "queued"

    def state(self):
        """The run's state: frame, paused, title_id, console, keyboard_waiting, wait_log,
        game_output_lines, exit_reason, run_folder, version."""
        reply = self._request(ZESTE_STATE, struct.pack("II", 0, 0))
        return json.loads(reply.decode("utf-8"))

    def game_output(self, since=0):
        """The game's debug output lines from index `since` on: (next_index, lines)."""
        lines = []
        while True:
            reply = self._request(ZESTE_OUTPUT, struct.pack("II", since, 0))
            first, nxt = struct.unpack("II", reply[:8])
            text = reply[8:].decode("utf-8", "replace")
            if not text:
                # Nothing new (lines older than the last 2000 are gone: start at `first`).
                return max(since, first), lines
            lines += text.split("\n")[:-1]
            since = nxt


if __name__ == "__main__":
    import sys
    rpc = ZesteRPC(timeout=2)
    rpc.command(" ".join(sys.argv[1:]) if len(sys.argv) > 1 else "log hello from ZesteRPC")
    print("queued")
