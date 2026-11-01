import os
from datetime import datetime, timezone
from pathlib import Path
from typing import List, Dict, Any
from fastapi import FastAPI, HTTPException, status
from fastapi.middleware.cors import CORSMiddleware
from fastapi.staticfiles import StaticFiles

from backend.config import settings
from backend.models import SensorReadingCreate, SensorReadingResponse
from backend.database import (
    insert_sensor_reading,
    get_latest_sensor_reading,
    get_recent_sensor_readings,
)

# Initialize FastAPI application
app = FastAPI(
    title="Automatic Car Wiper IoT API",
    version="1.0.0",
    description=(
        "REST API backend for ESP32 Automatic Car Wiper system. "
        "Receives real-time telemetry from ESP32 rain sensors and serves data to the web dashboard."
    ),
)

# Enable CORS for browser-based dashboard access from any domain
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)


@app.get(
    "/api/health",
    tags=["Health"],
    summary="Check whether the backend service is running",
)
def check_health() -> Dict[str, Any]:
    """
    Health check endpoint for Docker container checks and Render monitoring.
    Returns 200 OK along with service status and Supabase configuration status.
    """
    return {
        "status": "healthy",
        "service": "Automatic Car Wiper Backend",
        "supabase_configured": settings.is_supabase_configured,
        "timestamp": datetime.now(timezone.utc).isoformat(),
    }


@app.post(
    "/api/readings",
    tags=["Telemetry"],
    summary="Receive new sensor readings from ESP32",
    status_code=status.HTTP_201_CREATED,
)
def create_reading(reading: SensorReadingCreate) -> Dict[str, Any]:
    """
    Receives telemetry sent by the ESP32 hardware:
    - device_id: Unique device name (e.g., 'ESP32-WIPER')
    - rain_value: Rain sensor ADC reading (0 to 4095)
    - rain_status: State description ('Dry', 'Light Rain', 'Heavy Rain')
    - wiper_on: True if wiper servo is running, False if stopped
    - wiper_speed: Sweeping speed level ('Off', 'Slow', 'Fast')
    """
    payload = reading.model_dump()
    saved_record = insert_sensor_reading(payload)
    return {
        "message": "Sensor reading recorded successfully",
        "data": saved_record,
    }


@app.get(
    "/api/latest",
    tags=["Telemetry"],
    summary="Retrieve the most recent sensor reading",
)
def get_latest_reading() -> Dict[str, Any]:
    """
    Fetches the single most recent sensor record from Supabase.
    Returns 404 if no readings have been inserted yet.
    """
    latest = get_latest_sensor_reading()
    if not latest:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail="No sensor readings found in the database yet.",
        )
    return {
        "data": latest
    }


@app.get(
    "/api/readings",
    tags=["Telemetry"],
    summary="Retrieve the latest 30 sensor readings",
)
def get_recent_readings() -> Dict[str, Any]:
    """
    Fetches the last 30 readings from Supabase ordered by creation time descending.
    Used by the dashboard to draw trends and charts.
    """
    readings = get_recent_sensor_readings(limit=30)
    return {
        "count": len(readings),
        "data": readings,
    }


# ====================================================================
# Mount Frontend Static Dashboard (Serves on the same Render domain)
# ====================================================================
FRONTEND_DIR = Path(__file__).resolve().parent.parent / "frontend"
if FRONTEND_DIR.exists():
    app.mount("/", StaticFiles(directory=str(FRONTEND_DIR), html=True), name="frontend")
