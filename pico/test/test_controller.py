"""Drives the firmware's controller logic (src/controller.c) against a fake panel bus through ctypes."""

from __future__ import annotations

import ctypes

import pytest

IDLE, OPENED, FAILED, LOST, UIDS_READY = range(5)
TICK_US = 40_000
EXCHANGE = ctypes.CFUNCTYPE(
    ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_size_t,
    ctypes.c_uint32,
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_size_t,
)
TWO_SQUARES = [0xC3, 0x00, 0x93, 0x00, 0xC0, 0x00, 0x00, 0x40]
WALL = bytes.fromhex(
    "C1 00 00 C3 C3 C2 00 C3 00 00 93 00 00 C3 C2 00 00 00 00 C0 00 C1 00 00 C2 00 00 C2 00 00 00 00 00 C2 00 C2 00 00 00 00 40"
)


def crc16_arc(data: bytes) -> int:
    crc = 0
    for value in data:
        crc ^= value
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def bench_uid(index: int) -> bytes:
    return bytes(
        [0x2D + index, 0x10, 0x02, 0x11, 0x87, 0x04, 0x26, 0xAF, 0xCB, 0x3C, 0xB5, 0x5A, 0x05, 0x19, 0x00, index]
    )


class FakePanels:
    """Answers the way the bench squares did; tests change `layout`, `poll_reply` and `broken_uid_reads`."""

    def __init__(self, layout: list[int]):
        self.layout = layout
        self.poll_reply: list[int] | None = None  # None: 2 bytes per square in the layout
        self.broken_uid_reads: set[int] = set()
        self.hops: dict[int, int] = {}  # relay depth per square index; defaults to the index
        self.frames: list[list[int]] = []
        self.callback = EXCHANGE(self.exchange)

    def exchange(self, frame, length, _listen_ms, reply, reply_max):
        sent = [frame[i] for i in range(length)]
        self.frames.append(sent)
        if sent == [0x80]:
            answer = self.layout
        elif len(sent) == 4 and sent[0] == 0xF8 and sent[3] == 0x82:
            index = sent[1] | sent[2] << 8
            uid = bench_uid(index)
            crc = crc16_arc(uid)
            if index in self.broken_uid_reads:
                crc ^= 0xFFFF
            answer = [0x00] * self.hops.get(index, index) + [0x01, *uid, crc & 0xFF, crc >> 8]
        elif sent == [0xC0]:
            squares = sum(value & 0xF0 == 0xC0 for value in self.layout)
            answer = self.poll_reply if self.poll_reply is not None else [0x00] * (2 * squares)
        else:
            answer = []
        answer = answer[:reply_max]
        for i, value in enumerate(answer):
            reply[i] = value
        return len(answer)

    def sent_since(self, mark: int) -> list[list[int]]:
        return self.frames[mark:]


@pytest.fixture(scope="module")
def library(build_host_library):
    lib = build_host_library("controller.c")
    lib.controller_init.argtypes = [ctypes.c_void_p, ctypes.c_uint64]
    lib.controller_tick.argtypes = [ctypes.c_void_p, ctypes.c_uint64, EXCHANGE]
    lib.controller_tick.restype = ctypes.c_int
    lib.controller_set.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_uint8)]
    lib.controller_set.restype = ctypes.c_bool
    lib.controller_uid.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    lib.controller_uid.restype = ctypes.POINTER(ctypes.c_uint8)
    lib.controller_count_squares.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
    lib.controller_count_squares.restype = ctypes.c_size_t
    return lib


class Controller:
    # The struct stays opaque to the tests: a generously sized zeroed buffer, observed only through the bus.
    STORAGE_BYTES = 16_384

    def __init__(self, library, panels: FakePanels):
        self.lib = library
        self.panels = panels
        self.storage = ctypes.create_string_buffer(self.STORAGE_BYTES)
        library.controller_init(self.storage, 0)

    def tick(self, now_us: int) -> int:
        return self.lib.controller_tick(self.storage, now_us, self.panels.callback)

    def uid(self, index: int) -> bytes:
        return bytes(self.lib.controller_uid(self.storage, index)[:16])

    def set(self, index: int, rgbw: list[int]) -> bool:
        return self.lib.controller_set(self.storage, index, (ctypes.c_uint8 * 4)(*rgbw))


def test_counts_squares_on_the_wall(library):
    assert library.controller_count_squares(WALL, len(WALL)) == 13


def open_and_read_ids(controller: Controller, panels: FakePanels) -> None:
    now = 0
    assert controller.tick(now) == OPENED
    while True:
        event = controller.tick(now)
        now += TICK_US
        if event == UIDS_READY:
            return
        assert event == IDLE


def test_opens_session_then_reads_ids_one_per_tick(library):
    panels = FakePanels(TWO_SQUARES)
    controller = Controller(library, panels)
    assert controller.tick(0) == OPENED
    assert panels.frames == [[0x00], [0x80]]
    assert controller.tick(0) == IDLE
    assert panels.sent_since(2) == [[0xF8, 0x00, 0x00, 0x82], [0xC0]]
    assert controller.tick(TICK_US) == UIDS_READY
    assert panels.sent_since(4) == [[0xF8, 0x01, 0x00, 0x82], [0xC0]]
    assert controller.uid(0) == bench_uid(0) and controller.uid(1) == bench_uid(1)
    assert controller.tick(2 * TICK_US) == IDLE
    assert panels.sent_since(6) == [[0xC0]]


def test_bad_id_crc_is_retried_then_session_restarts(library):
    panels = FakePanels(TWO_SQUARES)
    panels.broken_uid_reads = {1}
    controller = Controller(library, panels)
    controller.tick(0)
    events = [controller.tick(TICK_US * step) for step in range(1, 8)]
    assert LOST in events
    assert UIDS_READY not in events


def test_pushes_one_entry_per_square_farthest_first(library):
    panels = FakePanels(TWO_SQUARES)
    controller = Controller(library, panels)
    open_and_read_ids(controller, panels)
    assert controller.set(0, [0xFF, 0x00, 0x00, 0x00])
    assert controller.set(1, [0x00, 0x00, 0xFF, 0x00])
    mark = len(panels.frames)
    controller.tick(10 * TICK_US)
    assert panels.sent_since(mark) == [
        [0xFC, 0x04, 0xFF],
        [0xE0, 0x01, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x05, 0x00, 0xFF, 0x00, 0x00, 0x00],
        [0xC0],
    ]


def test_ticks_every_40_ms(library):
    panels = FakePanels(TWO_SQUARES)
    controller = Controller(library, panels)
    open_and_read_ids(controller, panels)
    controller.tick(10 * TICK_US)
    mark = len(panels.frames)
    controller.tick(11 * TICK_US - 1)
    assert panels.sent_since(mark) == []
    controller.tick(11 * TICK_US)
    assert panels.sent_since(mark) == [[0xC0]]


@pytest.mark.parametrize("bad_poll", [[0xCC], [], [0x00] * 6])
def test_reopens_session_when_poll_is_wrong(library, bad_poll):
    panels = FakePanels(TWO_SQUARES)
    controller = Controller(library, panels)
    open_and_read_ids(controller, panels)
    panels.poll_reply = bad_poll
    assert controller.tick(10 * TICK_US) == LOST
    panels.poll_reply = None
    assert controller.tick(10 * TICK_US + 1) == OPENED
    assert panels.frames.count([0x80]) == 2


def test_retries_bad_layout_after_one_second(library):
    panels = FakePanels([0xC3, 0x00])  # no end marker
    controller = Controller(library, panels)
    assert controller.tick(0) == FAILED
    assert controller.tick(999_999) == IDLE
    panels.layout = TWO_SQUARES
    assert controller.tick(1_000_000) == OPENED


def test_rejects_out_of_range_index(library):
    controller = Controller(library, FakePanels(TWO_SQUARES))
    assert not controller.set(64, [0, 0, 0, 0])


def test_crc_matches_bench_reply(library):
    library.controller_crc16_arc.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
    library.controller_crc16_arc.restype = ctypes.c_uint16
    uid = bytes.fromhex("2D 10 02 11 87 04 26 AF CB 3C B5 5A 05 19 00 F5")
    assert library.controller_crc16_arc(uid, len(uid)) == 0x51AA  # trailer AA 51 on the bench


def test_reads_ids_when_hop_count_differs_from_index(library):
    # On the wall, square 5 sits two hops from the controller: hops follow tree depth, not bus order.
    panels = FakePanels(list(WALL))
    panels.hops = {index: min(index, 2) for index in range(13)}
    controller = Controller(library, panels)
    open_and_read_ids(controller, panels)
    assert controller.uid(5) == bench_uid(5)
    assert controller.uid(12) == bench_uid(12)
