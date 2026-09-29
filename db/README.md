# Chip database (v4 vendor-profile model)

Three layers, resolved and flattened on the host by `tools/chipdb.py`:

- `families/` — bus/command set shared by a memory type (`spi-nand`).
- `profiles/` — vendor quirks: ECC status decoding, quad-enable, OOB layout.
- `chips/` — one file per part: JEDEC id, geometry, and which family/profile it uses.

Design: `docs/superpowers/specs/2026-08-23-vendor-profile-architecture-design.md`.

**Status: stage 1 (host only).** The firmware still builds from `chips.yml`; a
test keeps the two in agreement until stage 2 moves the device onto this DB.

Check the DB and see what the device would receive:

```bash
python3 tools/chipdb.py                       # validate + list
python3 tools/chipdb.py --show MT29F2G01ABAGD # flattened profile
python3 tools/chipdb.py --blob MT29F2G01ABAGD # push blob, hex
```
