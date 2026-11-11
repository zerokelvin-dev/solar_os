"""E1001 hardware contracts: the 7.5-inch monochrome reTerminal."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from solaros_board_manifest import generate_header, load_board_manifest, load_driver_catalog, validate_board
from tests.test_foreground_presenter import production_functions


class E1001BoardTest(unittest.TestCase):
    def setUp(self):
        self.catalog = load_driver_catalog(ROOT / "boards/expansion_drivers.toml")
        self.board = load_board_manifest(ROOT / "boards/manifests/seeed_reterminal_e1001.toml")

    def test_native_usb_cannot_claim_the_i2c_pins(self):
        validate_board(self.board, self.catalog)
        self.assertNotIn("cdc", self.board["build"]["capabilities"])
        buses = {b["name"]: b for b in self.board["buses"]}
        self.assertEqual((buses["i2c0"]["sda"], buses["i2c0"]["scl"]), (19, 20))
        self.assertEqual((buses["i2c1"]["sda"], buses["i2c1"]["scl"]), (39, 40))
        self.assertEqual((buses["uart0"]["tx"], buses["uart0"]["rx"]), (43, 44))

    def test_usb_console_requires_explicit_start(self):
        self.assertNotIn("SOLAR_OS_BOARD_DEFAULT_PORT_SHELL", self.board["defines"])

    def test_display_uses_the_gdey075t7_panel_on_shared_spi(self):
        devices = {d["name"]: d for d in self.board["devices"]}
        display = devices["display0"]
        self.assertEqual(display["driver"], "uc8179")
        self.assertEqual(display["bindings"]["panel"], 2)
        self.assertEqual((display["bindings"]["cs"], display["bindings"]["dc"],
                          display["bindings"]["reset"], display["bindings"]["busy"]),
                         (10, 11, 12, 13))
        self.assertEqual(devices["storage0"]["bindings"]["cs"], 14)
        defines = self.board["defines"]
        self.assertEqual((defines["SOLAR_OS_BOARD_DISPLAY_WIDTH"],
                          defines["SOLAR_OS_BOARD_DISPLAY_HEIGHT"]), ("800", "480"))

    def test_buttons_match_the_e1003(self):
        defines = self.board["defines"]
        self.assertEqual(defines["SOLAR_OS_BOARD_PIN_KEY"], "GPIO_NUM_3")
        self.assertEqual(defines["SOLAR_OS_BOARD_PIN_BLE_PAIR"], "GPIO_NUM_4")
        self.assertEqual(defines["SOLAR_OS_BOARD_PIN_ESCAPE"], "GPIO_NUM_5")
        self.assertNotIn("SOLAR_OS_BOARD_PIN_APP_EXIT", defines)

    def test_connector_layout_is_complete_and_runtime_visible(self):
        layout = self.board["connector"]
        self.assertEqual((layout["rows"], layout["columns"]), (8, 1))
        expected = [
            ("power", None, "3V3"), ("ground", None, "GND"),
            ("gpio", 46, "GPIO46"), ("gpio", 2, "GPIO2/ADC"),
            ("gpio", 17, "TX1"), ("gpio", 18, "RX1"),
            ("gpio", 20, "SCL"), ("gpio", 19, "SDA"),
        ]
        entries = self.board["connectors"]
        self.assertEqual(len(entries), len(expected))
        for row, (entry, (kind, gpio, label)) in enumerate(zip(entries, expected)):
            self.assertEqual((entry["connector"], entry["position"], entry["row"], entry["column"]),
                             ("J2", row + 1, row, 0))
            self.assertEqual((entry["kind"], entry.get("gpio"), entry["label"]), (kind, gpio, label))

        header = generate_header(self.board, self.catalog)
        macros = [line for line in header.splitlines() if line.startswith((
            "#define SOLAR_OS_BOARD_CONNECTOR_LAYOUT_",
            "#define SOLAR_OS_BOARD_CONNECTOR_PIN_COUNT "))]
        source = '#include <assert.h>\n#include "solar_os_pins.h"\n' + "\n".join(macros) + "\n"
        source += production_functions((ROOT / "src/services/solar_os_pins.c").read_text(),
                                       ("solar_os_connector_layout_get_info",))
        source += r'''
int main(void) {
    solar_os_connector_layout_info_t layout;
    assert(solar_os_connector_layout_get_info(&layout));
    assert(layout.rows == 8 && layout.columns == 1);
    assert(SOLAR_OS_BOARD_CONNECTOR_PIN_COUNT == 8);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path, binary = Path(tmp, "layout.c"), Path(tmp, "layout")
            path.write_text(source)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-I", str(ROOT / "src/services"), "-I", str(ROOT / "include"),
                str(path), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
