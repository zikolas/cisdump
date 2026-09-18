"""Compile and run the hardware-free regression harness in a temporary directory."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
cc = shlex.split(os.environ.get("CC", "cc"))
common = ["-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(root / "tests/stubs")]

# Check the production source against its documented C89 language target.
subprocess.run(
    cc + common + ["-x", "c", "-std=c89", "-Dstricmp=strcmp", "-fsyntax-only", str(root / "CISDUMP.C")],
    check=True,
)
with tempfile.TemporaryDirectory(prefix="cisdump-tests-") as temp:
    binary = Path(temp) / "test_cisdump"
    subprocess.run(
        cc + common + ["-std=c99", "-fsanitize=address,undefined", "-g",
                       str(root / "tests/test_cisdump.c"), "-o", str(binary)],
        check=True,
    )
    subprocess.run([str(binary)], cwd=temp, check=True)
