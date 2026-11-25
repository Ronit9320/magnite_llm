from backend._features import BLUEPRINTS_AND_TOOLS
from backend._version import RELEASE_VERSION
import math

from fastapi import FastAPI
from fastapi.encoders import jsonable_encoder
from fastapi.exceptions import RequestValidationError
from backend.maintenance import UpdateGate
from backend.request_limits import BodyLimitMiddleware

from backend.routes.adapters import router as adapters_router, _adapter_setup
from backend.routes.blueprints import blueprint_setup, router as blueprints_router, shutdown_blueprints
from backend.routes.health import router as health_router
from backend.routes.system import router as system_router
from backend.routes.inference import router as inference_router
from backend.routes.models import router as models_router, _registry_setup
from backend.rag.store import StoreBusy
from backend.routes.rag import _rag_setup, rag_router
from backend.routes.tooling import router as tooling_router
from backend.routes.training import (
    datasets_router,
    training_router,
    _training_setup,
)

app = FastAPI(title="Magnite Space Backend", version=RELEASE_VERSION)
app.add_middleware(UpdateGate)
app.add_middleware(BodyLimitMiddleware)


def _json_safe(value):
    """Make a validation error body encodable: NaN/Infinity become text, lone surrogates become '?'."""
    if isinstance(value, float) and not math.isfinite(value):
        return str(value)
    if isinstance(value, str):
        return value.encode("utf-8", "replace").decode("utf-8")
    if isinstance(value, dict):
        return {key: _json_safe(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [_json_safe(item) for item in value]
    return value


@app.exception_handler(RequestValidationError)
async def invalid_request(request, exc):
    # FastAPI's default handler echoes the rejected input; NaN, Infinity (or a lone surrogate) in it
    # could not be encoded and turned a 422 into a 500.
    from fastapi.responses import JSONResponse
    return JSONResponse(status_code=422, content={"detail": _json_safe(jsonable_encoder(exc.errors()))})


@app.exception_handler(StoreBusy)
async def store_busy(request, exc):
    from fastapi.responses import JSONResponse
    return JSONResponse(status_code=503, content={"detail": str(exc)})


@app.on_event("shutdown")
async def shutdown():
    import asyncio
    from backend.inference.supervisor import get_supervisor
    from backend.routes.training import _get_manager
    await asyncio.to_thread(shutdown_blueprints)
    await asyncio.to_thread(_get_manager().shutdown)
    from backend.routes import models as model_routes
    if model_routes._registry is not None:
        await asyncio.to_thread(model_routes._registry.shutdown)
    await asyncio.to_thread(get_supervisor().unload)


@app.on_event("startup")
async def startup():
    from backend import runtime_packages
    runtime_packages.activate()   # user-installed packages must be importable before engines are probed
    _registry_setup()
    _training_setup()
    store = _adapter_setup()
    _rag_setup()
    if BLUEPRINTS_AND_TOOLS:   # switched-off releases never create blueprints.db
        blueprint_setup()
    from backend.inference.supervisor import get_supervisor

    get_supervisor().set_adapter_store(store)


app.include_router(health_router)
app.include_router(system_router)
app.include_router(models_router)
app.include_router(inference_router)
app.include_router(training_router)
app.include_router(datasets_router)
app.include_router(adapters_router)
app.include_router(rag_router)
if BLUEPRINTS_AND_TOOLS:
    app.include_router(blueprints_router)
    app.include_router(tooling_router)
