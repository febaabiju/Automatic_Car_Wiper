# ====================================================================
# Automatic Car Wiper - Docker Containerization
# Production container for FastAPI Backend & Frontend IoT Dashboard
# Target Platform: Render / Cloud Container Deployment
# ====================================================================

FROM python:3.11-slim

# Environment settings: unbuffered stdout, disable bytecode generation, default PORT
ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    PORT=10000

WORKDIR /app

# Install curl for container health check
RUN apt-get update && apt-get install -y --no-install-recommends \
    curl \
    && rm -rf /var/lib/apt/lists/*

# Install Python dependencies
COPY requirements.txt .
RUN pip install --no-cache-dir --upgrade pip && \
    pip install --no-cache-dir -r requirements.txt

# Copy application backend and frontend assets
COPY backend/ ./backend/
COPY frontend/ ./frontend/

# Expose default port (Render will dynamically assign PORT at runtime)
EXPOSE 10000

# Container healthcheck targeting the /api/health endpoint
HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
    CMD curl -f http://127.0.0.1:${PORT}/api/health || exit 1

# Start FastAPI server via Uvicorn bound to 0.0.0.0 and dynamic $PORT
CMD ["sh", "-c", "uvicorn backend.main:app --host 0.0.0.0 --port ${PORT:-10000}"]

