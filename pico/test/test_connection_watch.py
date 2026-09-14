"""When the wall signals the MQTT connection (src/connection_watch.c) through ctypes."""

from __future__ import annotations

import ctypes

import pytest

SECOND = 1_000_000
NONE, OK, PROBLEM = 0, 1, 2


@pytest.fixture(scope="module")
def lib(build_host_library):
    library = build_host_library("connection_watch.c")
    library.connection_watch_init.argtypes = [ctypes.c_void_p, ctypes.c_uint64]
    library.connection_watch_update.argtypes = [ctypes.c_void_p, ctypes.c_bool, ctypes.c_uint64]
    library.connection_watch_update.restype = ctypes.c_int
    return library


class Watch:
    STORAGE_BYTES = 64  # opaque to the tests; larger than connection_watch_t

    def __init__(self, lib, boot_us: int = 0):
        self.lib = lib
        self.storage = ctypes.create_string_buffer(self.STORAGE_BYTES)
        lib.connection_watch_init(self.storage, boot_us)

    def update(self, connected: bool, now_us: int) -> int:
        return self.lib.connection_watch_update(self.storage, connected, now_us)

    def signals(self, connected: bool, start_s: float, end_s: float) -> list[tuple[float, int]]:
        """Updates every 100 ms and returns the signals raised, with their time in seconds."""
        raised = []
        for step in range(round(start_s * 10), round(end_s * 10)):
            signal = self.update(connected, step * SECOND // 10)
            if signal != NONE:
                raised.append((step / 10, signal))
        return raised


def test_first_connection_signals_ok_once(lib):
    watch = Watch(lib)
    assert watch.signals(False, 0, 8) == []
    assert watch.signals(True, 8, 20) == [(8.0, OK)]


def test_no_connection_within_30_s_of_boot_signals_a_problem_once(lib):
    watch = Watch(lib)
    assert watch.signals(False, 0, 120) == [(30.0, PROBLEM)]


def test_recovering_from_a_signalled_problem_signals_ok(lib):
    watch = Watch(lib)
    watch.signals(False, 0, 40)
    assert watch.signals(True, 40, 50) == [(40.0, OK)]


def test_short_outage_after_connecting_signals_nothing(lib):
    watch = Watch(lib)
    watch.signals(True, 5, 10)
    assert watch.signals(False, 10, 39.9) == []
    assert watch.signals(True, 39.9, 60) == []


def test_long_outage_after_connecting_signals_a_problem_then_ok(lib):
    watch = Watch(lib)
    watch.signals(True, 5, 10)
    assert watch.signals(False, 10, 100) == [(40.0, PROBLEM)]
    assert watch.signals(True, 100, 110) == [(100.0, OK)]


def test_outage_timer_restarts_after_each_reconnect(lib):
    watch = Watch(lib)
    watch.signals(True, 0, 1)
    watch.signals(False, 1, 25)
    watch.signals(True, 25, 26)
    assert watch.signals(False, 26, 55) == []
    assert watch.signals(False, 55, 57) == [(56.0, PROBLEM)]
