"""
Robust HTTP client for ZW3D HTTP Server.

Wraps all endpoints with retry logic, timeouts, and structured error handling.
"""

import json
import requests
import time
import logging
from typing import Dict, Any, Optional

logger = logging.getLogger("zw3d_agent")

# Default configuration
DEFAULT_HOST = "localhost"
DEFAULT_PORT = 8081

# Timeout presets (seconds)
TIMEOUTS = {
    "status": 5,
    "analyze_part": 30,
    "inspect_views": 30,
    "evaluate_quality": 45,
    "generate_drafting": 120,
    "generate_smart": 180,
    "generate_native_layout": 180,
    "generate_optimized": 180,
    "regenerate": 180,
    "add_pmi": 60,
    "set_reference_profile": 30,
    "add_section": 60,
    "open_part": 90,
    "clear_views": 15,
    "create_block": 30,
}

# Retry configuration
MAX_RETRIES = 3
RETRY_BACKOFF = 2.0  # exponential backoff base


class ZW3DHTTPError(Exception):
    """Raised when a ZW3D HTTP request fails after all retries."""
    pass


class ZW3DHttpClient:
    """HTTP client for ZW3D HTTP Server with retry and timeout handling."""

    def __init__(self, host: str = DEFAULT_HOST, port: int = DEFAULT_PORT):
        self.base_url = f"http://{host}:{port}"
        self._session = requests.Session()

    def _request(self, method: str, endpoint: str, timeout_key: str,
                 json_data: Optional[Dict] = None, max_retries: int = MAX_RETRIES) -> Dict[str, Any]:
        """Send HTTP request with retry logic."""
        url = f"{self.base_url}{endpoint}"
        timeout = TIMEOUTS.get(timeout_key, 30)

        last_error = None
        for attempt in range(max_retries):
            try:
                if method == "GET":
                    resp = self._session.get(url, timeout=timeout)
                else:
                    # Use ensure_ascii=False so Chinese characters are sent as UTF-8
                    # instead of \uXXXX escapes that the C++ JsonStr parser can't handle
                    if json_data is not None:
                        body = json.dumps(json_data, ensure_ascii=False).encode("utf-8")
                        resp = self._session.post(url, data=body, timeout=timeout,
                                                  headers={"Content-Type": "application/json; charset=utf-8"})
                    else:
                        resp = self._session.post(url, timeout=timeout)
                resp.raise_for_status()
                result = resp.json()
                if result.get("status") == "error":
                    logger.warning("ZW3D returned error: %s", result.get("message", "unknown"))
                return result
            except requests.exceptions.ConnectionError as e:
                last_error = e
                if attempt < max_retries - 1:
                    wait = RETRY_BACKOFF ** attempt
                    logger.warning("Connection failed (attempt %d/%d), retrying in %.1fs: %s",
                                   attempt + 1, max_retries, wait, e)
                    time.sleep(wait)
            except requests.exceptions.Timeout as e:
                last_error = e
                if attempt < max_retries - 1:
                    wait = RETRY_BACKOFF ** attempt
                    logger.warning("Request timed out (attempt %d/%d), retrying in %.1fs",
                                   attempt + 1, max_retries, wait)
                    time.sleep(wait)
            except requests.exceptions.JSONDecodeError as e:
                logger.error("Invalid JSON response: %s", e)
                return {"status": "error", "message": f"Invalid JSON: {e}"}

        raise ZW3DHTTPError(f"Request failed after {max_retries} retries: {last_error}")

    def check_server(self) -> bool:
        """Check if ZW3D HTTP Server is running."""
        try:
            result = self._request("GET", "/status", "status", max_retries=1)
            return result.get("status") == "ok"
        except ZW3DHTTPError:
            return False

    def wait_for_server(self, max_wait: int = 120, poll_interval: int = 3) -> bool:
        """Wait for ZW3D HTTP Server to become ready."""
        start = time.time()
        while time.time() - start < max_wait:
            if self.check_server():
                logger.info("ZW3D HTTP Server is ready")
                return True
            time.sleep(poll_interval)
        logger.error("ZW3D HTTP Server not ready after %ds", max_wait)
        return False

    def analyze_part(self) -> Dict[str, Any]:
        """Analyze active part geometry and recommend base view/profile."""
        return self._request("GET", "/analyze_part", "analyze_part")

    def inspect_views(self) -> Dict[str, Any]:
        """Inspect active drawing views, paper bounds, and occupied rects."""
        return self._request("GET", "/inspect_views", "inspect_views")

    def evaluate_quality(self) -> Dict[str, Any]:
        """Evaluate active drawing against drafting quality rules."""
        return self._request("GET", "/evaluate_drawing_quality", "evaluate_quality")

    def generate_smart_drafting(self) -> Dict[str, Any]:
        """Generate smart 3-view drafting with auto PMI."""
        return self._request("POST", "/generate_smart_drafting", "generate_smart")

    def generate_native_view_layout(self, view_count: int = 0,
                                    paper: str = "") -> Dict[str, Any]:
        """Generate views using ZW3D's native cvxDwgViewLayout auto layout."""
        payload: Dict[str, Any] = {}
        if view_count > 0:
            payload["view_count"] = view_count
        if paper:
            payload["paper"] = paper
        return self._request("POST", "/generate_native_view_layout", "generate_native_layout",
                             json_data=payload)

    def generate_drafting(self) -> Dict[str, Any]:
        """Generate standard 3-view drafting."""
        return self._request("POST", "/generate_drafting", "generate_drafting")

    def generate_optimized_drafting(self) -> Dict[str, Any]:
        """Generate drafting with quality evaluation and remediation."""
        return self._request("POST", "/generate_optimized_drafting", "generate_optimized")

    def regenerate_drafting(self, paper_index_offset: int = 0,
                            scale_multiplier: float = 1.0) -> Dict[str, Any]:
        """Regenerate drafting with paper/scale overrides."""
        return self._request("POST", "/regenerate_drafting", "regenerate",
                             json_data={
                                 "paper_index_offset": paper_index_offset,
                                 "scale_multiplier": scale_multiplier,
                             })

    def add_pmi(self) -> Dict[str, Any]:
        """Add PMI dimensions to all views in the active drawing."""
        return self._request("POST", "/add_pmi", "add_pmi")

    def add_pmi_variant(self, mode: int = 0) -> Dict[str, Any]:
        """Add PMI dimensions with an experimental native auto-dimension mode."""
        return self._request("POST", "/add_pmi_variant", "add_pmi",
                             json_data={"mode": mode})

    def set_reference_profile(self, profile: Dict[str, Any]) -> Dict[str, Any]:
        """Set or clear the active learned reference profile."""
        return self._request("POST", "/set_reference_profile", "set_reference_profile",
                             json_data=profile)

    def add_section_view(self, label: str = "A", position: str = "below") -> Dict[str, Any]:
        """Add a full section view to the active drawing."""
        return self._request("POST", "/add_section_view", "add_section",
                             json_data={"label": label, "position": position})

    def open_part(self, path: str) -> Dict[str, Any]:
        """Open a ZW3D part file (.Z3PRT)."""
        return self._request("POST", "/open_part", "open_part",
                             json_data={"path": path})

    def clear_sheet_views(self) -> Dict[str, Any]:
        """Clear all views on the active drawing sheet."""
        return self._request("POST", "/clear_sheet_views", "clear_views")

    def create_block(self, length: float = 80, width: float = 60,
                     height: float = 40) -> Dict[str, Any]:
        """Create a block solid in the active part."""
        return self._request("POST", "/create_block", "create_block",
                             json_data={"length": length, "width": width, "height": height})
