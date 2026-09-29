import uvicorn
from fastapi import FastAPI

from backend.routes.adapters import router as adapters_router, _adapter_setup
from backend.routes.health import router as health_router
from backend.routes.inference import router as inference_router
from backend.routes.models import router as models_router, _registry_setup
from backend.routes.rag import _rag_setup, rag_router
from backend.routes.training import (
    datasets_router,
    training_router,
    _training_setup,
)

app = FastAPI(title="Magnite LLM Backend", version="0.1.0")


@app.on_event("shutdown")
async def shutdown():
    import asyncio
    from backend.inference.supervisor import get_supervisor
    from backend.routes.training import _get_manager
    await asyncio.to_thread(_get_manager().shutdown)
    await asyncio.to_thread(get_supervisor().unload)


@app.on_event("startup")
async def startup():
    _registry_setup()
    _training_setup()
    store = _adapter_setup()
    _rag_setup()
    from backend.inference.supervisor import get_supervisor

    get_supervisor().set_adapter_store(store)


app.include_router(health_router)
app.include_router(models_router)
app.include_router(inference_router)
app.include_router(training_router)
app.include_router(datasets_router)
app.include_router(adapters_router)
app.include_router(rag_router)
