# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Autonomy®

"""The fault in ``GET /api/status``.

The runtime answers ``FAULT:<json>`` with the first fault since the last start
(a stuck task, a task a runtime error ended, a safe-mode restart's reason).
``/api/status`` adds it as ``fault`` when there is one; an older runtime that
does not know FAULT, or no fault at all, leaves the reply as it was.
"""

import pytest


@pytest.fixture()
def app_module(app):
    from webserver import app as app_module

    return app_module


class _Runtime:
    """Answers each socket command with a fixed line; records what was sent."""

    def __init__(self, replies):
        self.replies = replies
        self.sent = []

    def send_and_receive(self, msg, timeout=0.5):
        self.sent.append(msg)
        return self.replies.get(msg.strip())


_FAULT = (
    'FAULT:{"code":16,"kind":"task-stuck","task":1,"taskName":"NET",'
    '"safeMode":false,"count":1,"detail":1000}\n'
)


def test_status_carries_the_fault(app_module, monkeypatch):
    fake = _Runtime({"STATUS": "STATUS:ERROR\n", "SWITCH": "SWITCH:RUN\n", "FAULT": _FAULT})
    monkeypatch.setattr(app_module.runtime_manager, "runtime_socket", fake)

    result = app_module.handle_status({})

    assert result["status"] == "STATUS:ERROR\n"
    assert result["fault"] == {
        "code": 16,
        "kind": "task-stuck",
        "task": 1,
        "taskName": "NET",
        "safeMode": False,
        "count": 1,
        "detail": 1000,
    }
    assert "FAULT\n" in fake.sent


def test_no_fault_leaves_the_status_as_it_was(app_module, monkeypatch):
    none = 'FAULT:{"code":0,"kind":"none","task":-1,"taskName":"","safeMode":false,"count":0,"detail":0}\n'
    fake = _Runtime({"STATUS": "STATUS:RUNNING\n", "SWITCH": "SWITCH:RUN\n", "FAULT": none})
    monkeypatch.setattr(app_module.runtime_manager, "runtime_socket", fake)

    assert "fault" not in app_module.handle_status({})


@pytest.mark.parametrize("reply", [None, "COMMAND:UNKNOWN\n", "FAULT:ERROR\n", "FAULT:{not json\n"])
def test_an_older_runtime_or_a_bad_reply_adds_nothing(app_module, monkeypatch, reply):
    fake = _Runtime({"STATUS": "STATUS:RUNNING\n", "SWITCH": "SWITCH:RUN\n", "FAULT": reply})
    monkeypatch.setattr(app_module.runtime_manager, "runtime_socket", fake)

    result = app_module.handle_status({})

    assert result["status"] == "STATUS:RUNNING\n"
    assert "fault" not in result
