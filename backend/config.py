import os
from pathlib import Path
from dotenv import load_dotenv

# Locate project root directory
BASE_DIR = Path(__file__).resolve().parent.parent

# Locate .env file in root directory
ENV_FILE = BASE_DIR / ".env"

# Load environment variables from .env if present
if ENV_FILE.exists():
    load_dotenv(dotenv_path=ENV_FILE)
else:
    load_dotenv()


class Settings:
    """Safe configuration reader for backend environment variables."""

    SUPABASE_URL: str = os.getenv("SUPABASE_URL", "").strip()
    SUPABASE_KEY: str = os.getenv("SUPABASE_KEY", "").strip()
    HOST: str = os.getenv("HOST", "0.0.0.0")
    # Defaults to 10000 for Render cloud hosting, or respects PORT from .env / environment
    PORT: int = int(os.getenv("PORT", "10000"))

    @property
    def is_supabase_configured(self) -> bool:
        """
        Validates whether real Supabase credentials have been configured.
        Returns False if credentials are empty or contain placeholder values.
        """
        if not self.SUPABASE_URL or not self.SUPABASE_KEY:
            return False
        if "your-project-id" in self.SUPABASE_URL or "your-supabase-anon" in self.SUPABASE_KEY:
            return False
        return True


settings = Settings()
