from __future__ import annotations

import ctypes
import pathlib
import shutil
import subprocess

import pytest

SOURCE_DIR = pathlib.Path(__file__).resolve().parent.parent / "src"


@pytest.fixture(scope="session")
def build_host_library(tmp_path_factory):
    """Compiles firmware C sources that have no Pico dependencies into a host shared library."""
    compiler = shutil.which("cc")
    if compiler is None:
        pytest.skip("no host C compiler")

    def build(*sources: str) -> ctypes.CDLL:
        library = tmp_path_factory.mktemp("host") / "libfirmware.so"
        paths = [str(SOURCE_DIR / source) for source in sources]
        subprocess.run(
            [compiler, "-O2", "-Wall", "-Werror", "-shared", "-fPIC", "-o", str(library), *paths], check=True
        )
        return ctypes.CDLL(str(library))

    return build
