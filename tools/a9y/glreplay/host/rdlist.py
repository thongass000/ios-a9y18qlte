#!/usr/bin/env python3
"""List the sections of a freedreno .rd(.gz) capture: buffers (gpuaddr, size) and cmdstreams."""
import gzip, struct, sys

NAMES = ["NONE", "TEST", "CMD", "GPUADDR", "CONTEXT", "CMDSTREAM", "CMDSTREAM_ADDR", "PARAM", "FLUSH",
         "PROGRAM", "VERT_SHADER", "FRAG_SHADER", "BUFFER_CONTENTS", "GPU_ID", "CHIP_ID",
         "SHADER_LOG_BUFFER", "CP_LOG_BUFFER", "WRBUFFER"]

def sections(path):
    data = (gzip.open if path.endswith(".gz") else open)(path, "rb").read()
    off = 0
    while off + 8 <= len(data):
        typ, sz = struct.unpack_from("<II", data, off)
        yield typ, data[off + 8:off + 8 + sz]
        off += 8 + sz

if __name__ == "__main__":
    for typ, body in sections(sys.argv[1]):
        name = NAMES[typ] if typ < len(NAMES) else str(typ)
        if typ == 3:
            lo, size = struct.unpack_from("<II", body)
            hi = struct.unpack_from("<I", body, 8)[0] if len(body) >= 12 else 0
            print(f"GPUADDR {hi << 32 | lo:#x} size {size:#x}")
        elif typ == 6:
            lo, dw = struct.unpack_from("<II", body)
            hi = struct.unpack_from("<I", body, 8)[0] if len(body) >= 12 else 0
            print(f"CMDSTREAM_ADDR {hi << 32 | lo:#x} dwords {dw}")
        elif typ == 12:
            nz = sum(1 for b in body if b)
            print(f"  BUFFER_CONTENTS {len(body):#x} bytes, {nz} non-zero")
        else:
            print(name, len(body), body[:40] if typ in (1, 2) else "")
