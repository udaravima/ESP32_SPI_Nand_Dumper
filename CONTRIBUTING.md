# Adding a NAND chip

The chip registry lives in [`chips.yml`](chips.yml). Adding your chip is a few
lines of YAML plus a pull request — no C required.

> **Heads up — a layered profile database is being built.** A data-driven
> `family → profile → chip` database under [`db/`](db/) has landed as host-side
> foundation (see the
> [Developer Guide](docs/DEVELOPER_GUIDE.md#vendorprofile-architecture-stage-1)).
> It is **not yet on the device path** — for a chip the firmware should detect and
> dump *today*, `chips.yml` below is still the place. Wiring the device onto `db/`
> is Stage 2; until then, add device chips here.

## Steps

1. **Read the JEDEC id.** Wire your chip per the [README](README.md) and boot
   with the serial monitor open. The log prints the manufacturer and device id,
   e.g. `Detected UNKNOWN (0x2C 0x24)` or your router's own probe line
   (`mfr_id=0x2c, dev_id=0x24`).

2. **Add a block to `chips.yml`** (copy an existing entry):

   ```yaml
     YOUR_PART_NAME:
       mfr_id:  0x2C          # from the serial log
       dev_id:  0x24
       page_size:       2176  # total bytes/page = main + spare (from datasheet)
       spare_size:      128
       pages_per_block: 64    # must be a power of two
       total_blocks:    2048
       bad_block_mark:  0x00  # value written to spare[0] of a bad block's page 0
       has_qe_bit:      false # true for Winbond/GigaDevice (see below)
       ecc_default:     off
       vcc_mv:          3300  # 1800 parts need a level shifter
       notes: "cite the datasheet here"
   ```

   For chips that need a Quad-Enable bit (Winbond W25N, GigaDevice GD5F), add:

   ```yaml
       has_qe_bit:      true
       qe_feature_addr: 0xB0
       qe_bit:          0x01
   ```

3. **Build.** `pio run -e esp32dev` regenerates `src/nand_chips_generated.h`
   from `chips.yml` and compiles. Entries are validated at build time — a
   malformed one fails the build with a message naming the offending field.

4. **Verify a dump**, then open a PR with the datasheet reference in `notes`.

## Running the tests

```bash
pip install -r requirements-dev.txt
python3 -m pytest        # host tools + wire-format cross-check
pio test -e native       # pure C logic
```
