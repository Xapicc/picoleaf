"""Home Assistant light JSON handling (src/ha_light.c) through ctypes."""

from __future__ import annotations

import ctypes
import json

import pytest


class LightState(ctypes.Structure):
    _fields_ = [
        ("on", ctypes.c_bool),
        ("brightness", ctypes.c_uint8),
        ("red", ctypes.c_uint8),
        ("green", ctypes.c_uint8),
        ("blue", ctypes.c_uint8),
    ]


class Discovery(ctypes.Structure):
    _fields_ = [
        (name, ctypes.c_char_p)
        for name in (
            "name",
            "unique_id",
            "command_topic",
            "state_topic",
            "availability_topic",
            "device_id",
            "firmware_version",
        )
    ]


@pytest.fixture(scope="module")
def lib(build_host_library):
    library = build_host_library("ha_light.c")
    library.ha_light_apply_command.argtypes = [ctypes.POINTER(LightState), ctypes.c_char_p, ctypes.c_size_t]
    library.ha_light_apply_command.restype = ctypes.c_bool
    library.ha_light_state_json.argtypes = [ctypes.POINTER(LightState), ctypes.c_char_p, ctypes.c_size_t]
    library.ha_light_state_json.restype = ctypes.c_size_t
    library.ha_light_output.argtypes = [ctypes.POINTER(LightState), ctypes.POINTER(ctypes.c_uint8)]
    library.ha_light_discovery_json.argtypes = [ctypes.POINTER(Discovery), ctypes.c_char_p, ctypes.c_size_t]
    library.ha_light_discovery_json.restype = ctypes.c_size_t
    return library


def default_state() -> LightState:
    return LightState(True, 128, 255, 255, 255)


def apply(lib, state: LightState, payload: str) -> bool:
    data = payload.encode()
    return lib.ha_light_apply_command(ctypes.byref(state), data, len(data))


def as_tuple(state: LightState) -> tuple:
    return (state.on, state.brightness, state.red, state.green, state.blue)


@pytest.mark.parametrize(
    ("payload", "expected"),
    [
        ('{"state":"OFF"}', (False, 128, 255, 255, 255)),
        ('{"state":"ON","brightness":40}', (True, 40, 255, 255, 255)),
        ('{"state": "ON", "color_mode": "rgb", "color": {"r": 255, "g": 0, "b": 10}}', (True, 128, 255, 0, 10)),
        ('{"brightness":255,"transition":2}', (True, 255, 255, 255, 255)),
    ],
)
def test_applies_home_assistant_commands(lib, payload, expected):
    state = default_state()
    assert apply(lib, state, payload)
    assert as_tuple(state) == expected


@pytest.mark.parametrize(
    "payload",
    ['{"state":"MAYBE"}', '{"brightness":256}', '{"color":{"r":1,"g":2}}', '{"effect":"rainbow"}', "", "not json"],
)
def test_rejects_bad_commands_without_changing_state(lib, payload):
    state = default_state()
    assert not apply(lib, state, payload)
    assert as_tuple(state) == as_tuple(default_state())


def test_state_json_round_trips_through_a_command(lib):
    state = LightState(True, 77, 1, 2, 3)
    buffer = ctypes.create_string_buffer(256)
    length = lib.ha_light_state_json(ctypes.byref(state), buffer, len(buffer))
    text = buffer.value.decode()
    assert length == len(text)
    assert json.loads(text) == {"state": "ON", "brightness": 77, "color_mode": "rgb", "color": {"r": 1, "g": 2, "b": 3}}
    copy = default_state()
    assert apply(lib, copy, text)
    assert as_tuple(copy) == as_tuple(state)


def test_state_json_reports_truncation(lib):
    buffer = ctypes.create_string_buffer(16)
    assert lib.ha_light_state_json(ctypes.byref(default_state()), buffer, len(buffer)) == 0


@pytest.mark.parametrize(
    ("state", "expected"),
    [
        (LightState(True, 255, 255, 128, 0), [255, 128, 0, 0]),
        (LightState(True, 128, 255, 255, 255), [128, 128, 128, 0]),
        (LightState(False, 255, 255, 255, 255), [0, 0, 0, 0]),
    ],
)
def test_output_scales_colour_by_brightness(lib, state, expected):
    rgbw = (ctypes.c_uint8 * 4)()
    lib.ha_light_output(ctypes.byref(state), rgbw)
    assert list(rgbw) == expected


def test_discovery_payload_is_valid_json(lib):
    light = Discovery(
        b"Tile 3",
        b"canvas_e6614c311b825937_2d100211870426afcb3cb55a051900f5",
        b"canvas/e6614c311b825937/2d100211870426afcb3cb55a051900f5/set",
        b"canvas/e6614c311b825937/2d100211870426afcb3cb55a051900f5/state",
        b"canvas/e6614c311b825937/status",
        b"canvas_e6614c311b825937",
        b"0.5.0",
    )
    buffer = ctypes.create_string_buffer(1024)
    assert lib.ha_light_discovery_json(ctypes.byref(light), buffer, len(buffer)) > 0
    payload = json.loads(buffer.value)
    assert payload["schema"] == "json"
    assert payload["supported_color_modes"] == ["rgb"]
    assert payload["device"]["identifiers"] == ["canvas_e6614c311b825937"]
    assert payload["unique_id"].endswith("051900f5")
