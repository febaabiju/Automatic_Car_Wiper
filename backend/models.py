from datetime import datetime
from typing import Optional
from pydantic import BaseModel, Field, field_validator


class SensorReadingCreate(BaseModel):
    """
    Pydantic schema to validate incoming sensor telemetry from ESP32.
    """
    device_id: str = Field(
        ...,
        min_length=1,
        max_length=50,
        description="Identifier of the sending ESP32 device",
        examples=["ESP32-WIPER"]
    )
    rain_value: int = Field(
        ...,
        ge=0,
        le=4095,
        description="12-bit ADC reading from rain sensor (0 - 4095)",
        examples=[1200]
    )
    rain_status: str = Field(
        ...,
        min_length=1,
        max_length=50,
        description="Human-readable rain status (e.g. Dry, Light Rain, Heavy Rain)",
        examples=["Heavy Rain"]
    )
    wiper_on: bool = Field(
        ...,
        description="Current wiper status: true if active, false if stopped",
        examples=[True]
    )
    wiper_speed: str = Field(
        ...,
        min_length=1,
        max_length=20,
        description="Current wiper speed (Off, Slow, Fast)",
        examples=["Fast"]
    )

    @field_validator("wiper_speed")
    @classmethod
    def normalize_wiper_speed(cls, value: str) -> str:
        """
        Normalize wiper speed to Title Case (Off, Slow, Medium, Fast)
        to prevent HTTP 422 errors due to case mismatch (e.g., 'OFF' vs 'Off').
        """
        cleaned = value.strip()
        lower = cleaned.lower()
        if lower in {"off", "stop", "stopped"}:
            return "Off"
        elif lower in {"slow", "low"}:
            return "Slow"
        elif lower in {"fast", "high"}:
            return "Fast"
        elif lower in {"medium", "med"}:
            return "Medium"
        return cleaned.title()

    @field_validator("rain_status")
    @classmethod
    def normalize_rain_status(cls, value: str) -> str:
        """
        Normalize rain status to Title Case (Dry, Light Rain, Heavy Rain).
        """
        cleaned = value.strip()
        lower = cleaned.lower()
        if "heavy" in lower:
            return "Heavy Rain"
        elif "light" in lower:
            return "Light Rain"
        elif "dry" in lower or "no rain" in lower:
            return "Dry"
        return cleaned.title()

    model_config = {
        "json_schema_extra": {
            "example": {
                "device_id": "ESP32-WIPER",
                "rain_value": 1200,
                "rain_status": "Heavy Rain",
                "wiper_on": True,
                "wiper_speed": "Fast"
            }
        }
    }


class SensorReadingResponse(SensorReadingCreate):
    """
    Pydantic schema for readings retrieved from Supabase, including auto-generated columns.
    """
    id: Optional[int] = Field(None, description="Database auto-incremented record ID")
    created_at: Optional[datetime] = Field(None, description="Timestamp when reading was inserted")

    model_config = {
        "from_attributes": True
    }
