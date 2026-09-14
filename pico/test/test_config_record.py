"""Device settings record (src/config_record.c) through ctypes."""

from __future__ import annotations

import ctypes

import pytest


class DeviceConfig(ctypes.Structure):
    _fields_ = [
        ("wifi_ssid", ctypes.c_char * 33),
        ("wifi_password", ctypes.c_char * 64),
        ("mqtt_host", ctypes.c_char * 64),
        ("mqtt_port", ctypes.c_uint16),
        ("mqtt_user", ctypes.c_char * 65),
        ("mqtt_password", ctypes.c_char * 65),
    ]


RECORD_SIZE = 4 + 2 + 2 + ctypes.sizeof(DeviceConfig) + 4


@pytest.fixture(scope="module")
def lib(build_host_library):
    library = build_host_library("config_record.c")
    library.config_defaults.argtypes = [ctypes.POINTER(DeviceConfig)]
    library.config_encode.argtypes = [ctypes.POINTER(DeviceConfig), ctypes.c_char_p, ctypes.c_size_t]
    library.config_encode.restype = ctypes.c_size_t
    library.config_decode.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(DeviceConfig)]
    library.config_decode.restype = ctypes.c_bool
    library.config_set_field.argtypes = [ctypes.POINTER(DeviceConfig), ctypes.c_char_p, ctypes.c_char_p]
    library.config_set_field.restype = ctypes.c_bool
    return library


def configured(lib) -> DeviceConfig:
    config = DeviceConfig()
    lib.config_defaults(ctypes.byref(config))
    for key, value in {
        "wifi_ssid": "Example Net",
        "wifi_password": "correct horse battery staple",
        "mqtt_host": "192.168.1.10",
        "mqtt_user": "canvas",
        "mqtt_password": "0123456789abcdef",
    }.items():
        assert lib.config_set_field(ctypes.byref(config), key.encode(), value.encode())
    return config


def test_defaults_to_mqtt_port_1883(lib):
    config = DeviceConfig()
    lib.config_defaults(ctypes.byref(config))
    assert config.mqtt_port == 1883 and config.wifi_ssid == b""


def test_record_round_trips(lib):
    record = ctypes.create_string_buffer(RECORD_SIZE)
    assert lib.config_encode(ctypes.byref(configured(lib)), record, len(record)) == RECORD_SIZE
    decoded = DeviceConfig()
    assert lib.config_decode(record.raw, RECORD_SIZE, ctypes.byref(decoded))
    assert decoded.wifi_ssid == b"Example Net" and decoded.mqtt_port == 1883 and decoded.mqtt_user == b"canvas"


def test_erased_flash_is_not_a_config(lib):
    decoded = DeviceConfig()
    assert not lib.config_decode(b"\xff" * RECORD_SIZE, RECORD_SIZE, ctypes.byref(decoded))


def test_corrupted_record_is_rejected(lib):
    record = ctypes.create_string_buffer(RECORD_SIZE)
    lib.config_encode(ctypes.byref(configured(lib)), record, len(record))
    damaged = bytearray(record.raw)
    damaged[20] ^= 0x01
    assert not lib.config_decode(bytes(damaged), RECORD_SIZE, ctypes.byref(DeviceConfig()))


@pytest.mark.parametrize(
    ("key", "value"),
    [
        ("wifi_ssid", "x" * 33),
        ("mqtt_port", "0"),
        ("mqtt_port", "70000"),
        ("mqtt_port", "18a3"),
        ("unknown", "value"),
    ],
)
def test_rejects_invalid_fields(lib, key, value):
    config = DeviceConfig()
    lib.config_defaults(ctypes.byref(config))
    assert not lib.config_set_field(ctypes.byref(config), key.encode(), value.encode())


def test_accepts_longest_ssid(lib):
    config = DeviceConfig()
    assert lib.config_set_field(ctypes.byref(config), b"wifi_ssid", b"x" * 32)
