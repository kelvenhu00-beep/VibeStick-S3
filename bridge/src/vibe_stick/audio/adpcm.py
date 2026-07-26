from __future__ import annotations

import struct

INDEX_TABLE = (-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8)
STEP_TABLE = (
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130,
    143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449,
    494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411,
    1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026,
    4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767,
)


def decode_ima_adpcm(payload: bytes, sample_count: int) -> bytes:
    if sample_count < 1 or len(payload) < 4:
        raise ValueError("Invalid IMA ADPCM payload")
    required = 4 + ((sample_count - 1) + 1) // 2
    if len(payload) != required:
        raise ValueError("IMA ADPCM payload length does not match sample count")
    predictor = int.from_bytes(payload[0:2], "little", signed=True)
    index = payload[2]
    if index > 88:
        raise ValueError("Invalid IMA ADPCM step index")
    output = bytearray(sample_count * 2)
    struct.pack_into("<h", output, 0, predictor)
    for position in range(1, sample_count):
        code_index = position - 1
        packed = payload[4 + code_index // 2]
        code = (packed >> 4) & 0x0F if code_index & 1 else packed & 0x0F
        step = STEP_TABLE[index]
        delta = step >> 3
        if code & 4:
            delta += step
        if code & 2:
            delta += step >> 1
        if code & 1:
            delta += step >> 2
        predictor += -delta if code & 8 else delta
        predictor = max(-32768, min(32767, predictor))
        index = max(0, min(88, index + INDEX_TABLE[code]))
        struct.pack_into("<h", output, position * 2, predictor)
    return bytes(output)
