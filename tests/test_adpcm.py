from vibe_stick.audio.adpcm import decode_ima_adpcm


def test_decode_silence() -> None:
    assert decode_ima_adpcm(b"\x00\x00\x00\x00\x00\x00", 5) == b"\x00\x00" * 5


def test_rejects_wrong_length() -> None:
    try:
        decode_ima_adpcm(b"\x00\x00\x00\x00", 4)
    except ValueError:
        return
    raise AssertionError("Expected invalid length to fail")
