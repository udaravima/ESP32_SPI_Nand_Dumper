# Adding a chip

Chips live in the chip database under [`db/`](db/README.md): one small YAML file
per part in `db/chips/`, pointing at a vendor profile in `db/profiles/` and a
bus family in `db/families/`. Adding your chip is a few lines of YAML plus a
pull request — no C required.

## Steps

1. **Read the JEDEC id.** Wire your chip per the [README](README.md) and boot
   with the serial monitor open. The log prints the manufacturer, device and
   third ID byte, e.g. `E_UNKNOWN_ID for chip 0x2C 0x24 0x2C`, or your router's
   own probe line (`mfr_id=0x2c, dev_id=0x24`).

2. **Pick the profile.** A profile holds what differs between vendors: how the
   ECC status register is decoded, the quad-enable bit, and the OOB layout with
   the bad-block marker. Use the existing one for your vendor (`micron`,
   `dosilicon`). If there isn't one, add `db/profiles/<vendor>.yml` from the
   vendor's datasheet; the named ECC schemes (`generic2`, `micron3`, `gd_uc`,
   `xtx4`, `xtx_g0xa`) are listed in [`tools/chipdb.py`](tools/chipdb.py).

3. **Add `db/chips/YOUR_PART.yml`** (copy an existing chip):

   ```yaml
   YOUR_PART:
     id: {mfr: 0x2C, dev: 0x24, dev2: null, onfi: null}   # from the serial log
     family: spi-nand
     profile: micron
     geometry: {page_size: 2176, spare_size: 128, pages_per_block: 64,
                total_blocks: 2048, planes: 1}
     read_mode: single
     vcc_mv: 3300          # 1800 parts need a level shifter
     resident: true        # compile into the firmware
     datasheet: "docs/datasheets/... or a URL"   # required
     notes: "where the chip came from"
   ```

   `page_size` is main + spare. For multi-plane chips (the datasheet's block
   address says a bit such as `RA6` "controls the plane selection", e.g. Micron
   MT29F2G01), set `planes: 2`; getting it wrong makes every odd block read back
   the other plane's data. If two chips share `mfr`/`dev`, set each one's `dev2`
   (the third ID byte) so the firmware can tell them apart; otherwise it refuses
   to guess (`E_AMBIGUOUS_ID`).

4. **Check it.** `python3 tools/chipdb.py` validates the whole database and
   lists what the device will receive. `pio run -e esp32dev` regenerates
   `src/nand_profiles_generated.h` from `db/` and compiles; a bad entry fails
   the build with a message naming the chip and the field.

5. **Verify a dump**, then open a PR.

Have a v3 `chips.yml` with your own chips? Convert it once:
`python3 tools/chips_yml_to_db.py chips.yml --profile micron`, then fill in each
new file's `datasheet`.

## Adding a SPI NOR chip

Most SPI NOR chips are already in `db/chips/spi-nor/`, imported from
[flashrom](https://github.com/flashrom/flashrom)'s chip tables. Those files are
generated: do not edit them. To refresh them from a newer flashrom:

```bash
git clone https://github.com/flashrom/flashrom /tmp/flashrom
python3 tools/import_flashrom.py /tmp/flashrom --dry-run -v   # what would change
python3 tools/import_flashrom.py /tmp/flashrom
python3 tools/chipdb.py && python3 tools/gen_profiles.py && python3 tools/gen_profiles.py --golden
```

Only facts are imported (ID, size, voltage, 4-byte addressing, flashrom's read
test result), each with a link to the flashrom source it came from.

To add or correct a chip by hand, write it in its own file under `db/chips/`
(not in a `flashrom-*.yml` file). A hand-written chip with the same ID wins over
the imported one on the next import:

```yaml
W25Q128JV:
  id: {mfr: 0xEF, dev: 0x40, dev2: 0x18}   # 9Fh: manufacturer, type, capacity
  family: spi-nor
  profile: nor-winbond     # nor-macronix, nor-gigadevice, nor-generic
  geometry: {size_kib: 16384}
  addr4: none              # above 16 MiB: native, enter or enter_wren
  vcc_mv: 3300
  resident: true
  datasheet: "a URL"       # or source:
```

## Running the tests

```bash
pip install -r requirements-dev.txt
python3 -m pytest        # host tools, chip DB, wire-format cross-checks
pio test -e native       # pure C logic, golden profile blobs, simulated NAND
```

GitHub Actions (`.github/workflows/ci.yml`) runs the same checks on every pull
request, plus firmware builds for `esp32dev` and `esp32-s3-devkitc-1` and a check
that `src/nand_profiles_generated.h` and the golden blobs match `db/`.
