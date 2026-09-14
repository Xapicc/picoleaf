"""Fades and effect switching (src/renderer.c) through ctypes."""

from __future__ import annotations

import ctypes

import pytest
from test_effects import EFFECTS
from test_ha_light import LightState
from test_layout_c import WALL, Position

SECOND = 1_000_000
Rgbw = ctypes.c_uint8 * 4


@pytest.fixture(scope="module")
def lib(build_host_library):
    library = build_host_library("renderer.c", "effects.c", "layout.c")
    library.layout_positions.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(Position), ctypes.c_size_t]
    library.renderer_init.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    library.renderer_set_layout.argtypes = [ctypes.c_void_p, ctypes.POINTER(Position), ctypes.c_size_t, ctypes.c_uint]
    library.renderer_fade_to.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(Rgbw),
        ctypes.c_size_t,
        ctypes.c_uint64,
        ctypes.c_uint64,
    ]
    library.renderer_start_effect.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.POINTER(LightState)]
    library.renderer_frame.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.POINTER(Rgbw)]
    library.renderer_frame.restype = ctypes.c_bool
    library.renderer_start_boot_shimmer.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(Rgbw),
        ctypes.c_size_t,
        ctypes.c_uint64,
        ctypes.c_uint64,
    ]
    library.renderer_blink.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint8), ctypes.c_uint]
    return library


class Renderer:
    STORAGE_BYTES = 8192  # opaque to the tests; comfortably larger than renderer_t

    def __init__(self, lib):
        self.lib = lib
        self.storage = ctypes.create_string_buffer(self.STORAGE_BYTES)
        lib.renderer_init(self.storage, 42)
        positions = (Position * 64)()
        self.count = lib.layout_positions(WALL, len(WALL), positions, 64)
        lib.renderer_set_layout(self.storage, positions, self.count, 2)

    def fade(self, colour: tuple[int, int, int, int], now_us: int, duration_us: int) -> None:
        targets = (Rgbw * self.count)(*[Rgbw(*colour)] * self.count)
        self.lib.renderer_fade_to(self.storage, targets, self.count, now_us, duration_us)

    def boot_shimmer(self, rest: tuple[int, int, int, int], now_us: int, duration_us: int) -> None:
        targets = (Rgbw * self.count)(*[Rgbw(*rest)] * self.count)
        self.lib.renderer_start_boot_shimmer(self.storage, targets, self.count, now_us, duration_us)

    def blink(self, now_us: int, colour: tuple[int, int, int, int] = (255, 255, 255, 0), times: int = 1) -> None:
        self.lib.renderer_blink(self.storage, now_us, Rgbw(*colour), times)

    def effect(self, name: str, base: LightState) -> None:
        self.lib.renderer_start_effect(self.storage, EFFECTS.index(name), ctypes.byref(base))

    def frame(self, now_us: int) -> list[tuple[int, ...]] | None:
        output = (Rgbw * 64)()
        if not self.lib.renderer_frame(self.storage, now_us, output):
            return None
        return [tuple(output[i]) for i in range(self.count)]


def test_nothing_is_pushed_before_the_first_command(lib):
    assert Renderer(lib).frame(SECOND) is None


def test_zero_duration_switches_immediately(lib):
    renderer = Renderer(lib)
    renderer.fade((255, 0, 0, 0), SECOND, 0)
    assert set(renderer.frame(SECOND)) == {(255, 0, 0, 0)}


def test_fade_is_linear_and_ends_on_target(lib):
    renderer = Renderer(lib)
    renderer.fade((200, 0, 0, 0), SECOND, 0)
    renderer.frame(SECOND)
    renderer.fade((0, 0, 200, 0), 2 * SECOND, 2 * SECOND)
    assert set(renderer.frame(2 * SECOND)) == {(200, 0, 0, 0)}
    assert set(renderer.frame(3 * SECOND)) == {(100, 0, 100, 0)}
    assert set(renderer.frame(4 * SECOND)) == {(0, 0, 200, 0)}
    assert set(renderer.frame(9 * SECOND)) == {(0, 0, 200, 0)}


def test_new_fade_starts_from_what_is_shown_mid_fade(lib):
    renderer = Renderer(lib)
    renderer.fade((0, 0, 0, 0), 0, 0)
    renderer.frame(0)
    renderer.fade((200, 0, 0, 0), SECOND, 2 * SECOND)
    assert set(renderer.frame(2 * SECOND)) == {(100, 0, 0, 0)}
    renderer.fade((0, 0, 0, 0), 2 * SECOND, SECOND)
    assert set(renderer.frame(2 * SECOND + SECOND // 2)) == {(50, 0, 0, 0)}


def test_effect_frames_animate_and_leaving_the_effect_fades_from_its_last_frame(lib):
    renderer = Renderer(lib)
    renderer.effect("Rainbow wave", LightState(True, 255, 255, 255, 255))
    first, later = renderer.frame(SECOND), renderer.frame(2 * SECOND)
    assert first != later and len(set(first)) > 1
    renderer.fade((0, 0, 0, 0), 2 * SECOND, SECOND)
    assert renderer.frame(2 * SECOND) == later
    assert set(renderer.frame(3 * SECOND)) == {(0, 0, 0, 0)}


def test_updating_effect_base_keeps_animation_running(lib):
    renderer = Renderer(lib)
    renderer.effect("Breathe", LightState(True, 255, 255, 0, 0))
    red = renderer.frame(SECOND)
    renderer.effect("Breathe", LightState(True, 255, 0, 0, 255))
    blue = renderer.frame(SECOND)
    assert [(r, b) for r, _, b, _ in red] == [(b, r) for r, _, b, _ in blue]


def test_solid_is_not_an_animated_effect(lib):
    renderer = Renderer(lib)
    renderer.effect("Solid", LightState(True, 255, 255, 255, 255))
    assert renderer.frame(SECOND) is None


WHITE = (255, 255, 255, 0)
REST = (128, 128, 128, 0)
FRAME = 40_000


def run(renderer: Renderer, start_us: int, end_us: int) -> list[tuple[int, list | None]]:
    return [(now, renderer.frame(now)) for now in range(start_us, end_us, FRAME)]


def test_boot_shimmer_settles_on_rest_then_stops_sending(lib):
    renderer = Renderer(lib)
    renderer.boot_shimmer(REST, 0, 5 * SECOND)
    shimmer = renderer.frame(2 * SECOND)
    assert all(green > red for red, green, _, _ in shimmer)
    frames = run(renderer, 2 * SECOND + FRAME, 8 * SECOND)
    sent = [(now, frame) for now, frame in frames if frame is not None]
    assert set(sent[-1][1]) == {REST}
    # One second of settling after the shimmer, then the wall is left to USB colour commands.
    assert 6 * SECOND <= sent[-1][0] < 6 * SECOND + FRAME
    assert all(frame is None for now, frame in frames if now > sent[-1][0])
    assert all(set(frame) != {WHITE} for _, frame in sent)


def test_blink_during_shimmer_waits_for_the_shimmer_to_end(lib):
    renderer = Renderer(lib)
    renderer.boot_shimmer(REST, 0, 5 * SECOND)
    renderer.blink(SECOND)
    frames = dict(run(renderer, SECOND, 7 * SECOND))
    white = [now for now, frame in frames.items() if frame is not None and set(frame) == {WHITE}]
    assert white and min(white) >= 5 * SECOND and max(white) < 5 * SECOND + 200_000
    assert frames[6 * SECOND + 80_000] is None


def test_blink_after_shimmer_flashes_then_returns_to_rest(lib):
    renderer = Renderer(lib)
    renderer.boot_shimmer(REST, 0, SECOND)
    run(renderer, 0, 3 * SECOND)
    renderer.blink(3 * SECOND)
    assert set(renderer.frame(3 * SECOND)) == {WHITE}
    assert set(renderer.frame(3 * SECOND + 200_000)) == {REST}
    assert renderer.frame(3 * SECOND + 240_000) is None


def test_home_assistant_command_ends_shimmer_and_cancels_queued_blink(lib):
    renderer = Renderer(lib)
    renderer.boot_shimmer(REST, 0, 5 * SECOND)
    renderer.blink(SECOND)
    renderer.fade((255, 0, 0, 0), 2 * SECOND, 0)
    assert all(set(frame) == {(255, 0, 0, 0)} for _, frame in run(renderer, 2 * SECOND, 8 * SECOND))


def test_boot_shimmer_is_ignored_once_home_assistant_has_taken_over(lib):
    renderer = Renderer(lib)
    renderer.fade((0, 0, 255, 0), 0, 0)
    renderer.boot_shimmer(REST, 0, 5 * SECOND)
    assert set(renderer.frame(SECOND)) == {(0, 0, 255, 0)}


def test_repeated_blink_alternates_with_the_underlying_colours(lib):
    red = (255, 0, 0, 0)
    renderer = Renderer(lib)
    renderer.fade(REST, 0, 0)
    renderer.blink(SECOND, red, 3)
    shown = [set(renderer.frame(SECOND + step * 100_000)) for step in range(12)]
    # 200 ms on, 200 ms off, three times; sampled every 100 ms.
    assert shown == [{red}] * 2 + [{REST}] * 2 + [{red}] * 2 + [{REST}] * 2 + [{red}] * 2 + [{REST}] * 2


def test_newer_blink_replaces_a_queued_one(lib):
    red = (255, 0, 0, 0)
    renderer = Renderer(lib)
    renderer.boot_shimmer(REST, 0, 5 * SECOND)
    renderer.blink(SECOND, red, 3)
    renderer.blink(2 * SECOND)
    frames = [frame for _, frame in run(renderer, 2 * SECOND, 7 * SECOND) if frame is not None]
    assert not any(set(frame) == {red} for frame in frames)
    assert any(set(frame) == {WHITE} for frame in frames)


def test_blink_with_nothing_to_return_to_is_ignored(lib):
    renderer = Renderer(lib)
    renderer.blink(SECOND)
    assert renderer.frame(SECOND) is None
