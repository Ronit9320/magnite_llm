from backend._version import RELEASE_VERSION
from fastapi import APIRouter
from pydantic import BaseModel

router = APIRouter(tags=["health"])


class HealthResponse(BaseModel):
    status: str
    version: str


@router.get("/health", response_model=HealthResponse)
async def health_check():
    return HealthResponse(status="ok", version=RELEASE_VERSION)
