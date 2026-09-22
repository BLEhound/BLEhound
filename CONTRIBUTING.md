# Contributing to BLEhound

Thanks for your interest. BLEhound is firmware + host tooling + open hardware, so
contributions span C, Python, and KiCad.

## Ways to help

- **Test coverage across stacks.** The connection-following and multi-channel paths
  are exercised against specific central/peripheral stacks. Reports (or fixes) from
  other phones, SoCs, and BLE stacks are especially valuable.
- **Bug reports.** Include: firmware commit, board (nRF54LM20A / nRF52840), how you
  reproduced it, and a capture (`.pcapng`) if relevant. Scrub any private addresses.
- **Firmware / host code.** Keep changes focused; match the existing style.
- **Docs.** The documentation site lives in the separate `BLEhound.github.io` repo.

## Development setup

- **Firmware:** nRF Connect SDK v3.4.0 via `west` (see [firmware/README.md](firmware/README.md)).
- **Host:** Python 3.9+ with `pip install -r host/requirements.txt`. Run the
  aggregator's self-test: `python3 host/tri_aggregator.py --selftest`.

## Pull requests

1. Branch from `main`.
2. Keep commits conventional (`feat:`, `fix:`, `docs:`, `refactor:`, `test:`).
3. For firmware changes, confirm it builds: `west build -b nrf54lm20dk/nrf54lm20a/cpuapp -s firmware`.
4. For host changes, keep `--selftest` green.
5. Describe what you tested on real hardware.

## Responsible use

BLEhound is a security research and development tool. Only capture or analyze
traffic from devices you own or are authorized to test. Do not submit captures,
keys, or addresses belonging to third parties.

## License of contributions

By contributing you agree that your code is licensed under Apache-2.0 and your
hardware changes under CERN-OHL-S-2.0, matching the rest of the project.

## Contact

Questions, security reports, or Code of Conduct concerns: open an issue, or email
woowill007@gmail.com.
