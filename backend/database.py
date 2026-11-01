from typing import List, Optional, Dict, Any
from fastapi import HTTPException, status
from supabase import create_client, Client
from backend.config import settings

# Global Supabase client instance (cached singleton)
_supabase_client: Optional[Client] = None


def get_supabase_client() -> Client:
    """
    Initializes and returns the Supabase client.
    Raises HTTP 503 if credentials are missing or placeholder values are detected.
    """
    global _supabase_client

    if not settings.is_supabase_configured:
        raise HTTPException(
            status_code=status.HTTP_503_SERVICE_UNAVAILABLE,
            detail=(
                "Supabase is not configured yet. Please copy .env.example to .env "
                "and provide your real SUPABASE_URL and SUPABASE_KEY."
            )
        )

    if _supabase_client is None:
        try:
            _supabase_client = create_client(settings.SUPABASE_URL, settings.SUPABASE_KEY)
        except Exception as exc:
            raise HTTPException(
                status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
                detail=f"Failed to connect to Supabase: {str(exc)}"
            )

    return _supabase_client


def insert_sensor_reading(data: Dict[str, Any]) -> Dict[str, Any]:
    """
    Inserts a validated sensor reading dictionary into the 'sensor_readings' table.
    """
    client = get_supabase_client()
    try:
        response = client.table("sensor_readings").insert(data).execute()
        if response.data and len(response.data) > 0:
            return response.data[0]
        return data
    except HTTPException:
        raise
    except Exception as exc:
        raise HTTPException(
            status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
            detail=f"Database error while inserting reading: {str(exc)}"
        )


def get_latest_sensor_reading() -> Optional[Dict[str, Any]]:
    """
    Fetches the single most recent sensor reading from the 'sensor_readings' table.
    """
    client = get_supabase_client()
    try:
        response = (
            client.table("sensor_readings")
            .select("*")
            .order("created_at", desc=True)
            .limit(1)
            .execute()
        )
        if response.data and len(response.data) > 0:
            return response.data[0]
        return None
    except HTTPException:
        raise
    except Exception as exc:
        raise HTTPException(
            status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
            detail=f"Database error while fetching latest reading: {str(exc)}"
        )


def get_recent_sensor_readings(limit: int = 30) -> List[Dict[str, Any]]:
    """
    Fetches the latest readings up to 'limit' (default: 30) ordered by created_at descending.
    """
    client = get_supabase_client()
    try:
        response = (
            client.table("sensor_readings")
            .select("*")
            .order("created_at", desc=True)
            .limit(limit)
            .execute()
        )
        return response.data if response.data else []
    except HTTPException:
        raise
    except Exception as exc:
        raise HTTPException(
            status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
            detail=f"Database error while fetching readings history: {str(exc)}"
        )

