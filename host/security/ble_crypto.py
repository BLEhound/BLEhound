#!/usr/bin/env python3
"""Cryptographic primitives used by BLE pairing / encryption.

Authorized use only: these primitives back the pairing-crack / decryption tooling;
use them only on devices you own or are authorized to test. See
host/security/README.md ("Responsible use").

Based on the Bluetooth Core Spec:
  - Vol 3, Part H, §2.2: the pairing c1 / s1 functions (Legacy pairing)
  - Vol 6, Part E, §1: AES-CCM used for LE link-layer encryption

Every function is self-tested against the official test vectors from the BT spec
(see __main__ / test). This is the foundation for phases B/C (pairing crack /
key-injection decryption); a single wrong bit breaks everything downstream, so
test it thoroughly first.

Dependency: cryptography (pip3 install cryptography).
"""

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes


def aes128_ecb(key: bytes, block: bytes) -> bytes:
    """Single-block AES-128 encryption (i.e. e(key, block) in the spec). key/block are both 16 bytes, big-endian."""
    assert len(key) == 16 and len(block) == 16
    enc = Cipher(algorithms.AES(key), modes.ECB()).encryptor()
    return enc.update(block) + enc.finalize()


def _xor(a: bytes, b: bytes) -> bytes:
    return bytes(x ^ y for x, y in zip(a, b))


def c1(tk: bytes, rand: bytes, preq: bytes, pres: bytes,
       iat: int, ia: bytes, rat: int, ra: bytes) -> bytes:
    """Legacy pairing confirm-value function c1 (Spec Vol 3 Part H 2.2.3).

    c1 = e(tk, e(tk, rand XOR p1) XOR p2)
      p1 = pres || preq || rat || iat   (16 bytes, pres in the most significant position)
      p2 = 0x00000000 || ia || ra        (16 bytes)

    All arguments use the byte order from the spec (big-endian 16/7/6-byte values).
    """
    p1 = pres + preq + bytes([rat]) + bytes([iat])         # 7+7+1+1 = 16
    p2 = b"\x00\x00\x00\x00" + ia + ra                     # 4+6+6 = 16
    assert len(p1) == 16 and len(p2) == 16

    res = aes128_ecb(tk, _xor(rand, p1))
    res = aes128_ecb(tk, _xor(res, p2))
    return res


def s1(tk: bytes, r1: bytes, r2: bytes) -> bytes:
    """Legacy pairing short-term-key generation function s1 (Spec Vol 3 Part H 2.2.4).

    s1 = e(tk, r'),  r' = low 64 bits of r1 || low 64 bits of r2
    """
    r_prime = r1[8:16] + r2[8:16]
    return aes128_ecb(tk, r_prime)


def tk_from_passkey(passkey: int) -> bytes:
    """6-digit numeric passkey → 128-bit TK (big-endian integer of the number, high bytes zero-padded)."""
    return passkey.to_bytes(16, "big")


# ---------------------------------------------------------------- self-test

def _selftest() -> bool:
    ok = True

    # c1 official test vector (Spec Vol 3 Part H 2.2.3)
    tk = bytes(16)
    rand = bytes.fromhex("5783D52156AD6F0E6388274EC6702EE0")
    preq = bytes.fromhex("07071000000101")
    pres = bytes.fromhex("05000800000302")
    iat, rat = 0x01, 0x00
    ia = bytes.fromhex("A1A2A3A4A5A6")
    ra = bytes.fromhex("B1B2B3B4B5B6")
    expect_c1 = bytes.fromhex("1e1e3fef878988ead2a74dc5bef13b86")
    got_c1 = c1(tk, rand, preq, pres, iat, ia, rat, ra)
    print(f"c1: {'PASS' if got_c1 == expect_c1 else 'FAIL'}  {got_c1.hex()}")
    ok = ok and got_c1 == expect_c1

    # s1 official test vector
    r1 = bytes.fromhex("000F0E0D0C0B0A091122334455667788")
    r2 = bytes.fromhex("010203040506070899AABBCCDDEEFF00")
    expect_s1 = bytes.fromhex("9a1fe1f0e8b0f49b5b4216ae796da062")
    got_s1 = s1(tk, r1, r2)
    print(f"s1: {'PASS' if got_s1 == expect_s1 else 'FAIL'}  {got_s1.hex()}")
    ok = ok and got_s1 == expect_s1

    return ok


if __name__ == "__main__":
    import sys

    print("== BLE crypto primitives self-test (BT official test vectors)")
    sys.exit(0 if _selftest() else 1)
