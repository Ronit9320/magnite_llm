"""Reject oversized request bodies before they are parsed or held in memory."""
import json

DEFAULT_MAX_BYTES = 32 * 1024 ** 2


class _TooLarge(Exception):
    pass


class BodyLimitMiddleware:
    """ASGI middleware: answers 413 for a body over ``max_bytes``.

    A declared Content-Length is checked up front; a chunked body (no Content-Length) is counted
    as it arrives, so the handler is never fed more than the limit either way.
    """

    def __init__(self, app, max_bytes: int = DEFAULT_MAX_BYTES):
        self.app = app
        self.max_bytes = max_bytes

    async def __call__(self, scope, receive, send):
        if scope["type"] != "http":
            await self.app(scope, receive, send)
            return
        declared = dict(scope.get("headers", [])).get(b"content-length", b"")
        if declared.isdigit() and int(declared) > self.max_bytes:
            await self._reject(send)
            return

        received = 0
        started = False

        async def counted_receive():
            nonlocal received
            message = await receive()
            if message["type"] == "http.request":
                received += len(message.get("body", b""))
                if received > self.max_bytes:
                    raise _TooLarge()
            return message

        async def tracked_send(message):
            nonlocal started
            if message["type"] == "http.response.start":
                started = True
            await send(message)

        try:
            await self.app(scope, counted_receive, tracked_send)
        except _TooLarge:
            if not started:
                await self._reject(send)

    async def _reject(self, send):
        body = json.dumps({"detail": f"Request body too large (limit {self.max_bytes // 1024 ** 2} MiB)"}).encode()
        await send({"type": "http.response.start", "status": 413,
                    "headers": [(b"content-type", b"application/json"),
                                (b"content-length", str(len(body)).encode())]})
        await send({"type": "http.response.body", "body": body})
