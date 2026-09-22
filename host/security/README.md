> **English** | [中文](README.zh-CN.md)

# Security tooling

Cryptographic helpers for BLE link analysis. These are standard sniffer
capabilities (equivalent to `crackle`) and are provided for authorized security
research and interoperability testing.

- `ble_crypto.py` — BLE pairing / encryption primitives (AES-128, the `c1`/`s1`
  functions for Legacy pairing, AES-CCM for LE link encryption). Verified against
  the Bluetooth Core Spec test vectors.
- `crack_legacy.py` — passive Legacy-pairing recovery: from a captured pairing it
  brute-forces the Temporary Key (TK) to derive the STK, enabling decryption of
  that session.

## Important limits

- Only **Legacy pairing** (BLE 4.0/4.1) is passively breakable.
- **LE Secure Connections** (BLE 4.2+, P-256 ECDH) is **mathematically not**
  passively breakable — no sniffer can do it. If you have the LTK by other legitimate
  means, you can still decrypt by feeding it to Wireshark.

## Responsible use

Use these only on devices you own or are explicitly authorized to test. Capturing,
decrypting, or storing traffic from third-party devices without authorization may be
illegal in your jurisdiction. You are responsible for how you use these tools.
