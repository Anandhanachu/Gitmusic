"""
backend/__init__.py

Load .env file automatically when the backend package is imported.
"""
import os
from pathlib import Path

# Walk up until we find a .env file (supports running from project root or backend/)
_here = Path(__file__).parent
for _candidate in [_here / ".env", _here.parent / ".env"]:
    if _candidate.is_file():
        try:
            from dotenv import load_dotenv
            load_dotenv(_candidate)
        except ImportError:
            pass  # python-dotenv not installed; env vars must be set manually
        break
