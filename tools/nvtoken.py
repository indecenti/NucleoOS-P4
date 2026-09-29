"""The board's session token for PC tools: NUCLEO_TOKEN if set, else the file tools/pair.py writes
(%USERPROFILE%\\.nucleo\\token on Windows, ~/.nucleo/token elsewhere)."""
import os


def token_path():
    return os.path.join(os.path.expanduser("~"), ".nucleo", "token")


def token():
    t = os.environ.get("NUCLEO_TOKEN", "").strip()
    if t:
        return t
    try:
        with open(token_path(), encoding="utf-8") as f:
            return f.read().strip()
    except OSError:
        return ""


def auth_headers():
    """{"Authorization": "Bearer <token>"}, or {} when this PC is not paired yet."""
    t = token()
    return {"Authorization": "Bearer " + t} if t else {}
