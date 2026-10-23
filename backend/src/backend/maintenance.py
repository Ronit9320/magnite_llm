"""An update gate that drains HTTP work before the desktop stops the backend.

ASGI accounting includes streaming responses and worker cleanup. No new work is
accepted after quiescing. Background training/pulls are checked separately.
This is lifecycle coordination, not an authentication boundary.
"""
import json


class UpdateGate:
    def __init__(self, app):
        self.app = app
        self.active = 0
        self.quiesced = False

    def background_busy(self):
        from backend.routes.training import _get_manager
        from backend.routes.models import _get_registry

        if any(j.status in ("queued", "running") for j in _get_manager().list()):
            return "Training is queued or running. Finish it before updating."
        if _get_registry().has_active_pulls():
            return "A model download is in progress. Finish it before updating."
        return ""

    async def __call__(self, scope, receive, send):
        if scope["type"] != "http":
            return await self.app(scope, receive, send)
        path = scope["path"]
        if path in ("/updates/quiesce", "/updates/resume") and scope["method"] == "POST":
            if path.endswith("resume"):
                self.quiesced = False
                return await self.respond(send, 200, "Update cancelled; work can continue.")
            # No await between the checks and closing the gate. All HTTP
            # admission is serialized on the ASGI event loop.
            busy = "An operation is still in progress. Wait before restarting." if self.active else self.background_busy()
            if busy:
                return await self.respond(send, 409, busy)
            self.quiesced = True
            return await self.respond(send, 200, "Ready to restart.")
        if path == "/health":
            return await self.app(scope, receive, send)
        if self.quiesced:
            return await self.respond(send, 503, "Magnite is preparing to update.")
        self.active += 1
        try:
            await self.app(scope, receive, send)
        finally:
            self.active -= 1

    @staticmethod
    async def respond(send, status, detail):
        body = json.dumps({"detail": detail}).encode()
        await send({"type": "http.response.start", "status": status,
                    "headers": [(b"content-type", b"application/json")]})
        await send({"type": "http.response.body", "body": body})
