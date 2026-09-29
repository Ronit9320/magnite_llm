"""Cooperative cancellation for long HTTP operations running in worker threads."""
import asyncio
import threading
from contextlib import suppress

import anyio


class ClientDisconnected(RuntimeError):
    pass


async def watch_disconnect(request, stopped):
    # Explicit stop avoids racing cancellation against Request.is_disconnected's
    # internal AnyIO cancellation scope.
    while not stopped.is_set():
        if await request.is_disconnected():
            return
        try:
            await asyncio.wait_for(stopped.wait(), .1)
        except TimeoutError:
            pass


async def request_work(request, function):
    cancelled = threading.Event()
    stopped = asyncio.Event()
    work = asyncio.create_task(asyncio.to_thread(function, cancelled))
    watcher = asyncio.create_task(watch_disconnect(request, stopped))
    try:
        done, _ = await asyncio.wait((work, watcher), return_when=asyncio.FIRST_COMPLETED)
        if work in done:
            return await work
        cancelled.set()
        raise ClientDisconnected("Client disconnected; operation cancelled")
    except asyncio.CancelledError:
        cancelled.set()
        raise
    finally:
        stopped.set()
        # Native calls cannot be forcibly interrupted. Keep ownership until they
        # exit; the worker checks cancelled before publishing the result.
        with anyio.CancelScope(shield=True):
            await watcher
            with suppress(Exception):
                await asyncio.shield(work)
