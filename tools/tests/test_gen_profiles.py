import copy
import os
import re

import pytest

from tools import gen_profiles as gp

chipdb = gp.chipdb   # the module gen_profiles imported (PlatformIO-style path)


@pytest.fixture(scope="module")
def db():
    return chipdb.load_db()


def _read(path):
    with open(path) as fh:
        return fh.read()


def test_committed_resident_header_is_up_to_date(db):
    assert _read(gp.DEFAULT_OUT) == gp.render_header(db), \
        "run: python3 tools/gen_profiles.py"


def test_committed_golden_blobs_are_up_to_date(db):
    assert _read(gp.GOLDEN_OUT) == gp.render_golden(db), \
        "run: python3 tools/gen_profiles.py --golden"


def test_golden_header_bytes_are_the_push_blob(db):
    text = _read(gp.GOLDEN_OUT)
    body = re.search(r"GOLDEN_DS35Q1GA\[\d+\] = \{(.*?)\};", text, re.S).group(1)
    got = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-F]{2})", body))
    assert got == chipdb.pack_blob(chipdb.flatten(db, "DS35Q1GA"))
    assert chipdb.unpack_blob(got)["name"] == "DS35Q1GA"


def test_only_resident_chips_are_emitted(db):
    d = copy.deepcopy(db)
    d["chips"]["DS35Q1GA"]["resident"] = False
    out = gp.render_header(d)
    assert '"MT29F2G01ABAGD"' in out and '"DS35Q1GA"' not in out


def test_initializer_has_one_value_per_struct_byte_group(db):
    # Every LAYOUT field appears once per row, in order: count the scalar and
    # array slots of one row and compare with the layout.
    row = gp._row(chipdb.flatten(db, "DS35Q1GA"))
    body = row.split("\n", 1)[1]
    arrays = re.findall(r"\{([^{}]*)\}", body)
    scalars = re.sub(r"\{[^{}]*\}", "", body).replace('"DS35Q1GA"', "N").split(",")
    scalars = [s for s in (x.strip() for x in scalars) if s and s != "}"]
    layout = [f for f, fmt in chipdb.LAYOUT if fmt not in ("16s", "8H")]
    # +1: the explicit _pad0 is in LAYOUT as "x", the trailing _pad1 is an array
    assert len(scalars) == len(layout)
    assert [len(a.split(",")) for a in arrays] == [16, 8, 8, 2]


def test_bad_chip_fails_generation(db):
    d = copy.deepcopy(db)
    d["chips"]["DS35Q1GA"]["geometry"]["page_size"] = 16384
    with pytest.raises(chipdb.ChipDBError, match="MAX_PAGE_BUFFER"):
        gp.render_header(d)


def test_no_resident_chip_fails_generation(db):
    d = copy.deepcopy(db)
    for c in d["chips"].values():
        c["resident"] = False
    with pytest.raises(chipdb.ChipDBError, match="resident"):
        gp.render_header(d)


def test_max_page_buffer_matches_firmware():
    main = _read(os.path.join(chipdb.REPO, "src", "main.cpp"))
    assert f"#define MAX_PAGE_SIZE {chipdb.MAX_PAGE_BUFFER}" in main
