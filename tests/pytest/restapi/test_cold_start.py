# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Autonomy®

"""The cold-restart and retain-status endpoints.

``GET /api/cold-start-plc`` is START as a COLD restart (IEC 61131-3 Figure 9
rule 4): every variable, RETAIN included, at its initial value, and the stored
retained values overwritten. ``GET /api/retain-status`` reports what the last
start did with retained values. Both are thin: the runtime decides and answers
over its command socket, so what these tests pin is the wiring -- the socket
command each one sends, that the reply reaches the caller unchanged, that a bad
reply is reported rather than invented, and that both sit behind the same JWT
check as start-plc.
"""

import socket

import pytest

from conftest import auth


@pytest.fixture()
def app_module(app):
    from webserver import app as app_module
    from webserver import restapi

    # app.py registers its dispatchers in run_https(), which tests never call.
    restapi.register_callback_get(app_module.restapi_callback_get)
    return app_module


class _FakeSocket:
    """Records what the runtime manager sends, answers with a fixed line."""

    def __init__(self, reply=None, error=None):
        self.sent = []
        self.reply = reply
        self.error = error

    def send_and_receive(self, msg, timeout=0.5):
        self.sent.append(msg)
        if self.error is not None:
            raise self.error
        return self.reply


def test_both_endpoints_are_registered(app_module):
    assert app_module.GET_HANDLERS["cold-start-plc"] is app_module.handle_cold_start_plc
    assert app_module.GET_HANDLERS["retain-status"] is app_module.handle_retain_status


def test_cold_start_sends_cold_start_and_passes_the_reply_through(app_module, monkeypatch):
    fake = _FakeSocket(reply="COLD_START:OK\n")
    monkeypatch.setattr(app_module.runtime_manager, "runtime_socket", fake)

    assert app_module.handle_cold_start_plc({}) == {"status": "COLD_START:OK\n"}
    assert fake.sent == ["COLD_START\n"]


@pytest.mark.parametrize("error", [OSError("gone"), socket.error("gone"), RuntimeError("x")])
def test_cold_start_with_no_runtime_is_an_error_reply(app_module, monkeypatch, error):
    monkeypatch.setattr(app_module.runtime_manager, "runtime_socket", _FakeSocket(error=error))

    assert app_module.handle_cold_start_plc({}) == {"status": "COLD_START:ERROR\n"}


def test_retain_status_decodes_the_runtime_json(app_module, monkeypatch):
    reply = (
        'RETAIN:{"start":"warm","format":2,"store":"/x/retain.bin","blob_bytes":20,'
        '"stored_bytes":30,"result":7,"result_name":"migrated","restored":true,'
        '"stored_format":2,"kept":3,"converted":2,"truncated":1,"added":5,"dropped":4,'
        '"refused":0,"stored_layout":"0000abcd","program_layout":"12345678"}\n'
    )
    fake = _FakeSocket(reply=reply)
    monkeypatch.setattr(app_module.runtime_manager, "runtime_socket", fake)

    result = app_module.handle_retain_status({})

    assert fake.sent == ["RETAIN\n"]
    assert result["retain"]["result_name"] == "migrated"
    assert result["retain"]["kept"] == 3
    assert result["retain"]["stored_layout"] == "0000abcd"


@pytest.mark.parametrize(
    "reply", [None, "RETAIN:ERROR\n", "COMMAND:BUSY\n", "RETAIN:{not json}\n", "RETAIN:[1]\n"]
)
def test_an_unusable_retain_reply_is_reported_not_invented(app_module, monkeypatch, reply):
    monkeypatch.setattr(app_module.runtime_manager, "runtime_socket", _FakeSocket(reply=reply))

    assert app_module.handle_retain_status({}) == {"error": "No retain status from runtime"}


@pytest.mark.parametrize("endpoint", ["/api/cold-start-plc", "/api/retain-status"])
def test_both_need_a_token_like_start_plc(client, app_module, endpoint):
    assert client.get(endpoint).status_code == 401
    assert client.get("/api/start-plc").status_code == 401


def test_cold_start_through_the_route(client, app_module, admin_token, monkeypatch):
    fake = _FakeSocket(reply="COLD_START:ERROR_ALREADY_RUNNING\n")
    monkeypatch.setattr(app_module.runtime_manager, "runtime_socket", fake)

    resp = client.get("/api/cold-start-plc", headers=auth(admin_token))

    assert resp.status_code == 200
    assert resp.get_json() == {"status": "COLD_START:ERROR_ALREADY_RUNNING\n"}
    assert fake.sent == ["COLD_START\n"]
