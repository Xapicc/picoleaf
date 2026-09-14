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
