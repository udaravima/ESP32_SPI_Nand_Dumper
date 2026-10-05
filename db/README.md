# Chip database (v4 vendor-profile model)

Three layers, resolved and flattened on the host by `tools/chipdb.py`:

- `families/` — bus/command set shared by a memory type (`spi-nand`, `spi-nor`).
- `profiles/` — vendor quirks: ECC status decoding, quad-enable, OOB layout.
- `chips/` — one file per part: JEDEC id, geometry, and which family/profile it uses.
  `chips/spi-nor/flashrom-*.yml` are generated from flashrom by
  `tools/import_flashrom.py` (facts plus a source link per entry; do not edit).
  A chip written by hand elsewhere under `chips/` wins over an imported one.

Design: `docs/superpowers/specs/2026-08-23-vendor-profile-architecture-design.md`,
and for SPI NOR `docs/superpowers/specs/2026-10-05-spi-nor-design.md`.

**Status: stage 3.** The firmware builds from this DB: every chip tagged
`resident: true` is flattened by `tools/gen_profiles.py` into
`src/nand_profiles_generated.h` on each build, and the device reads its opcodes,
ECC decode, quad-enable bit and bad-block marker from that profile. A chip that
is in the DB but not resident needs no reflash: `dump.py` looks up the ID the
device detected, pushes the flat profile, checks the device's echo of it and
arms it. The device re-runs every check and refuses a profile whose expected ID
doesn't match the chip in the socket.

Check the DB and see what the device would receive:

```bash
python3 tools/chipdb.py                       # validate + summary
python3 tools/chipdb.py --list --family spi-nor  # every NOR chip
python3 tools/chipdb.py --show MT29F2G01ABAGD # flattened profile
python3 tools/chipdb.py --blob MT29F2G01ABAGD # push blob, hex
```
