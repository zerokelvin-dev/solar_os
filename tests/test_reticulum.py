"""Reticulum and LXMF host tests: the protocol codecs, the job, and the
service on the real microReticulum (see the Reticulum section of
tests/host/Makefile)."""
import os
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / "tests/host"
MBEDTLS = Path(os.environ.get(
    "RETICULUM_MBEDTLS_DIR",
    Path.home() / ".platformio/packages/framework-espidf/components/mbedtls/mbedtls"))
ENV = {**os.environ, "ASAN_OPTIONS": "detect_leaks=0", "UBSAN_OPTIONS": "halt_on_error=1"}


def build_and_run(test, target):
    jobs = str(max(1, (os.cpu_count() or 2) // 2))
    built = subprocess.run(["make", "-C", str(HOST), "-j", jobs, target],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    test.assertEqual(built.returncode, 0, built.stdout[-4000:])
    ran = subprocess.run([str(HOST / target)], stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True, timeout=120, env=ENV)
    test.assertEqual(ran.returncode, 0, ran.stdout[-4000:])


class ReticulumProtocolTest(unittest.TestCase):
    def test_hdlc(self):
        build_and_run(self, "reticulum_hdlc_test")

    def test_lora_frame(self):
        build_and_run(self, "reticulum_lora_frame_test")

    def test_lxmf_format(self):
        build_and_run(self, "lxmf_format_test")



class ReticulumJobTest(unittest.TestCase):
    def test_failed_start_and_stop_during_a_slow_pass(self):
        build_and_run(self, "reticulum_job_test")


class ReticulumBuildTest(unittest.TestCase):
    def test_host_build_takes_the_firmware_resource_limit(self):
        define = "RNS_RESOURCE_MAX_TRANSFER_SIZE=131072"
        self.assertIn(define, (ROOT / "src/CMakeLists.txt").read_text())
        self.assertIn("-D" + define, (HOST / "Makefile").read_text())


@unittest.skipUnless((MBEDTLS / "library/sha256.c").is_file(),
                     "needs an mbedTLS 3.x source tree (RETICULUM_MBEDTLS_DIR)")
class ReticulumServiceTest(unittest.TestCase):
    def test_service_and_job_on_microreticulum(self):
        build_and_run(self, "reticulum_service_test")



if __name__ == "__main__":
    unittest.main()
