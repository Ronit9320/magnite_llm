"""Run blocking token producers off the event loop with cooperative cancellation."""
import asyncio
import threading
import anyio


async def run_blocking(function, *args):
    """Keep engine state reserved until a cancelled blocking call really exits."""
    work = asyncio.create_task(asyncio.to_thread(function, *args))
    try:
        return await asyncio.shield(work)
    except asyncio.CancelledError:
        with anyio.CancelScope(shield=True):
            try:
                await asyncio.shield(work)
            except Exception:
                pass
        raise


async def threaded_stream(producer):
    loop = asyncio.get_running_loop()
    queue = asyncio.Queue()
    stop = threading.Event()
    done = object()

    def send(value):
        loop.call_soon_threadsafe(queue.put_nowait, value)

    def run():
        iterator = None
        try:
            iterator = producer(stop)
            for text in iterator:
                if stop.is_set():
                    break
                send(text)
        except Exception as exc:
            send(exc)
        finally:
            try:
                if iterator is not None:
                    iterator.close()
            except Exception as exc:
                send(exc)
            finally:
                send(done)

    worker = threading.Thread(target=run, daemon=True, name="inference-stream")
    worker.start()
    try:
        while True:
            item = await queue.get()
            if item is done:
                break
            if isinstance(item, Exception):
                raise item
            yield item
    finally:
        stop.set()
        # Do not allow model unload until its last generation worker has exited.
        with anyio.CancelScope(shield=True):
            await asyncio.to_thread(worker.join)
