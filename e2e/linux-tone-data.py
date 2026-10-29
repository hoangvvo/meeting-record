"""Raw float32 mono tone for a native PipeWire pw-cat client."""
import math
import os
import struct
import sys

# 100ms contains exactly 55 cycles, so repeating the block preserves phase.
block = struct.pack("<4800f", *(0.15 * math.sin(2 * math.pi * 550 * i / 48000) for i in range(4800)))
try:
    while True:
        sys.stdout.buffer.write(block)
        sys.stdout.buffer.flush()
except BrokenPipeError:
    os._exit(0)
