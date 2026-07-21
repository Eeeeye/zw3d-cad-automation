#!/usr/bin/env python3
"""
ZW3D MCP (Model Context Protocol) Client

This module provides tools for interacting with ZW3D CAD software
through an HTTP server plugin.
"""

import json
import requests
from typing import Dict, Any, Optional

# ZW3D HTTP Server configuration
ZW3D_HTTP_HOST = "localhost"
ZW3D_HTTP_PORT = 8081
ZW3D_BASE_URL = f"http://{ZW3D_HTTP_HOST}:{ZW3D_HTTP_PORT}"


class ZW3DClient:
    """Client for communicating with ZW3D HTTP Server"""
    
    def __init__(self, host: str = ZW3D_HTTP_HOST, port: int = ZW3D_HTTP_PORT):
        self.base_url = f"http://{host}:{port}"
        self.timeout = 30
    
    def _post(self, endpoint: str, data: Dict[str, Any]) -> Dict[str, Any]:
        """Send POST request to ZW3D server"""
        url = f"{self.base_url}{endpoint}"
        try:
            response = requests.post(
                url,
                json=data,
                timeout=self.timeout
            )
            response.raise_for_status()
            return response.json()
        except requests.exceptions.ConnectionError:
            return {"status": "error", "message": "Cannot connect to ZW3D HTTP Server. Is ZW3D running with the plugin loaded?"}
        except requests.exceptions.Timeout:
            return {"status": "error", "message": "Request timed out"}
        except Exception as e:
            return {"status": "error", "message": str(e)}

    def _get(self, endpoint: str) -> Dict[str, Any]:
        """Send GET request to ZW3D server"""
        url = f"{self.base_url}{endpoint}"
        try:
            response = requests.get(url, timeout=self.timeout)
            response.raise_for_status()
            return response.json()
        except requests.exceptions.ConnectionError:
            return {"status": "error", "message": "Cannot connect to ZW3D HTTP Server. Is ZW3D running with the plugin loaded?"}
        except requests.exceptions.Timeout:
            return {"status": "error", "message": "Request timed out"}
        except Exception as e:
            return {"status": "error", "message": str(e)}
    
    def generate_3views_with_pmi(self, sheet_name: str = "Sheet1", scale: float = 1.0) -> Dict[str, Any]:
        """
        Generate three orthographic views (front, top, right) with automatic dimensions.

        Args:
            sheet_name: Name of the drawing sheet to create
            scale: View scale ratio (default 1.0)

        Returns:
            Dictionary with status and message
        """
        params = {
            "sheet_name": sheet_name,
            "scale": scale,
            "views": ["front", "top", "right"],
            "auto_dimension": True
        }

        return self._post("/generate_drafting", params)

    def analyze_part(self) -> Dict[str, Any]:
        """Analyze the active part and recommend a base view/layout profile."""
        return self._get("/analyze_part")

    def inspect_drawing_views(self) -> Dict[str, Any]:
        """Inspect active drawing view counts, paper bounds, and occupied rectangles."""
        return self._get("/inspect_views")

    def evaluate_drawing_quality(self) -> Dict[str, Any]:
        """Evaluate the active drawing against measurable drafting quality rules."""
        return self._get("/evaluate_drawing_quality")

    def generate_smart_3views_with_pmi(self) -> Dict[str, Any]:
        """
        Analyze the active part and generate an adapted 3-view drawing with PMI.
        """
        return self._post("/generate_smart_drafting", {})

    def generate_optimized_drafting(self) -> Dict[str, Any]:
        """
        Generate smart drafting, evaluate it against quality rules, and run basic remediation when possible.
        """
        return self._post("/generate_optimized_drafting", {})

    def add_pmi_dimensions(self) -> Dict[str, Any]:
        """
        Add PMI (Product Manufacturing Information) dimensions to all views in the active drawing.

        This function automatically adds dimensions, tolerances, and annotations
        to the current drawing sheet using ZW3D's built-in auto-dimensioning capabilities.

        Returns:
            Dictionary with status and message
        """
        return self._post("/add_pmi", {})
    
    def execute_command(self, command: str) -> Dict[str, Any]:
        """
        Execute a ZW3D command.

        Args:
            command: ZW3D command string

        Returns:
            Dictionary with status and message
        """
        return self._post("/execute", {"command": command})

    def open_part(self, path: str) -> Dict[str, Any]:
        """
        Open a ZW3D part file (.Z3PRT) in the running ZW3D instance.

        Args:
            path: Absolute path to the .Z3PRT file

        Returns:
            Dictionary with status, root name, and active path
        """
        return self._post("/open_part", {"path": path})

    def clear_sheet_views(self) -> Dict[str, Any]:
        """
        Clear all views on the active drawing sheet.

        Returns:
            Dictionary with status and cleared_count
        """
        return self._post("/clear_sheet_views", {})

    def regenerate_drafting(self, paper_index_offset: int = 0,
                            scale_multiplier: float = 1.0) -> Dict[str, Any]:
        """
        Regenerate drafting with paper size and scale overrides.

        Args:
            paper_index_offset: Offset to add to the computed paper preset index (positive = larger paper)
            scale_multiplier: Multiplier to apply to the computed drawing scale (e.g. 0.9 = 10% smaller)

        Returns:
            Dictionary with generation result
        """
        return self._post("/regenerate_drafting", {
            "paper_index_offset": paper_index_offset,
            "scale_multiplier": scale_multiplier,
        })

    def add_section_view(self, label: str = "A", position: str = "below") -> Dict[str, Any]:
        """
        Add a full section view to the active drawing.

        Args:
            label: Section view label (e.g. "A", "B")
            position: Where to place the section view ("below" or "right")

        Returns:
            Dictionary with status and section view location
        """
        return self._post("/add_section_view", {"label": label, "position": position})

    def run_drafting_agent(self, part_path: str = None, max_iterations: int = 5,
                           pass_score: float = 85.0) -> Dict[str, Any]:
        """
        Run the full drafting agent loop for iterative part annotation and drafting.

        The agent analyzes the part, generates drafting, evaluates quality, and
        applies corrective actions in a loop until quality passes or max iterations.

        Args:
            part_path: Path to .Z3PRT file. If None, uses the currently active part.
            max_iterations: Maximum agent loop iterations (default 5)
            pass_score: Quality score threshold to pass (default 85, auto-adjusted per profile)

        Returns:
            Dictionary with agent result (status, score, iterations, history)
        """
        import sys
        import os
        sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
        from ZW3D_Agent.http_client import ZW3DHttpClient
        from ZW3D_Agent.zw3d_agent import ZW3DAgent, AgentConfig
        from dataclasses import asdict

        agent_client = ZW3DHttpClient(self.base_url.split("//")[1].split(":")[0],
                                       int(self.base_url.split(":")[-1]))
        config = AgentConfig(max_iterations=max_iterations, pass_score=pass_score)
        agent = ZW3DAgent(client=agent_client, config=config)
        result = agent.run(part_path=part_path)
        return asdict(result)


# Global client instance
_zw3d_client: Optional[ZW3DClient] = None


def get_zw3d_client() -> ZW3DClient:
    """Get or create global ZW3D client instance"""
    global _zw3d_client
    if _zw3d_client is None:
        _zw3d_client = ZW3DClient()
    return _zw3d_client


def generate_3views_with_pmi(sheet_name: str = "Sheet1", scale: float = 1.0) -> str:
    """
    Tool function: Generate three orthographic views with automatic dimensions in ZW3D.
    
    This function creates a new drawing sheet with three standard orthographic views
    (front, top, right) and automatically adds dimensions to each view.
    
    Args:
        sheet_name: Name of the drawing sheet to create (default: "Sheet1")
        scale: View scale ratio (default: 1.0)
        
    Returns:
        JSON string with operation result
        
    Example:
        >>> result = generate_3views_with_pmi("MyDrawing", 1.0)
        >>> print(result)
        {"status": "ok", "message": "Drafting with three views and dimensions created successfully", "views": 3}
    """
    client = get_zw3d_client()
    result = client.generate_3views_with_pmi(sheet_name, scale)
    return json.dumps(result, indent=2)


def execute_zw3d_command(command: str) -> str:
    """
    Tool function: Execute a ZW3D command.

    Args:
        command: ZW3D command string to execute

    Returns:
        JSON string with operation result
    """
    client = get_zw3d_client()
    result = client.execute_command(command)
    return json.dumps(result, indent=2)


def analyze_active_part() -> str:
    """
    Tool function: Analyze the active ZW3D part and recommend a base view/layout profile.
    """
    client = get_zw3d_client()
    result = client.analyze_part()
    return json.dumps(result, indent=2)


def inspect_drawing_views() -> str:
    """
    Tool function: Inspect active drawing views, occupied rectangles, and paper bounds.
    """
    client = get_zw3d_client()
    result = client.inspect_drawing_views()
    return json.dumps(result, indent=2)


def evaluate_drawing_quality() -> str:
    """
    Tool function: Check whether the active drawing meets measurable drafting rules and return optimization guidance.
    """
    client = get_zw3d_client()
    result = client.evaluate_drawing_quality()
    return json.dumps(result, indent=2)


def generate_smart_3views_with_pmi() -> str:
    """
    Tool function: Analyze the active part and generate an adapted 3-view drawing with PMI.
    """
    client = get_zw3d_client()
    result = client.generate_smart_3views_with_pmi()
    return json.dumps(result, indent=2)


def generate_optimized_drafting() -> str:
    """
    Tool function: Generate smart drafting, inspect quality, and return optimization results.
    """
    client = get_zw3d_client()
    result = client.generate_optimized_drafting()
    return json.dumps(result, indent=2)


def add_pmi_dimensions() -> str:
    """
    Tool function: Add PMI dimensions to all views in the active ZW3D drawing.

    This function automatically adds Product Manufacturing Information (PMI) including
    dimensions, tolerances, and annotations to the current drawing sheet.
    It uses ZW3D's built-in auto-dimensioning capabilities for intelligent dimension placement.

    Returns:
        JSON string with operation result

    Example:
        >>> result = add_pmi_dimensions()
        >>> print(result)
        {"status": "ok", "message": "PMI dimensions added successfully"}
    """
    client = get_zw3d_client()
    result = client.add_pmi_dimensions()
    return json.dumps(result, indent=2)


def open_zw3d_part(path: str) -> str:
    """
    Tool function: Open a ZW3D part file (.Z3PRT) in the running ZW3D instance.

    Args:
        path: Absolute path to the .Z3PRT file

    Returns:
        JSON string with operation result
    """
    client = get_zw3d_client()
    result = client.open_part(path)
    return json.dumps(result, indent=2)


def clear_drawing_views() -> str:
    """
    Tool function: Clear all views on the active ZW3D drawing sheet.

    Returns:
        JSON string with operation result including cleared count
    """
    client = get_zw3d_client()
    result = client.clear_sheet_views()
    return json.dumps(result, indent=2)


def regenerate_drafting(paper_index_offset: int = 0, scale_multiplier: float = 1.0) -> str:
    """
    Tool function: Regenerate ZW3D drafting with paper size and scale overrides.

    Args:
        paper_index_offset: Offset to add to computed paper preset index (positive = larger paper)
        scale_multiplier: Multiplier for computed drawing scale (e.g. 0.9 = 10% smaller)

    Returns:
        JSON string with operation result
    """
    client = get_zw3d_client()
    result = client.regenerate_drafting(paper_index_offset, scale_multiplier)
    return json.dumps(result, indent=2)


def add_section_view(label: str = "A", position: str = "below") -> str:
    """
    Tool function: Add a full section view to the active ZW3D drawing.

    Args:
        label: Section view label (e.g. "A", "B")
        position: Where to place the section view ("below" or "right")

    Returns:
        JSON string with operation result
    """
    client = get_zw3d_client()
    result = client.add_section_view(label, position)
    return json.dumps(result, indent=2)


def run_drafting_agent(part_path: str = None, max_iterations: int = 5,
                       pass_score: float = 85.0) -> str:
    """
    Tool function: Run the iterative drafting agent that analyzes a ZW3D part,
    generates drafting, evaluates quality, and applies corrections in a loop
    until quality passes or max iterations are reached.

    The agent handles different part types (blocky, plate-like, elongated, stepped)
    with appropriate strategies for paper size, section views, and PMI.

    Args:
        part_path: Path to .Z3PRT file. If None, uses the currently active part.
        max_iterations: Maximum agent loop iterations (default 5)
        pass_score: Quality score threshold to pass (default 85, auto-adjusted per profile)

    Returns:
        JSON string with agent result (status, score, iterations, history)
    """
    client = get_zw3d_client()
    result = client.run_drafting_agent(part_path, max_iterations, pass_score)
    return json.dumps(result, indent=2)


# MCP Tool definitions for model registration
MCP_TOOLS = [
    {
        "name": "analyze_active_part",
        "description": "Analyze the active ZW3D part geometry and recommend a base view/profile for drafting based on the part bounding box.",
        "parameters": {
            "type": "object",
            "properties": {}
        }
    },
    {
        "name": "generate_smart_3views_with_pmi",
        "description": "Analyze the active ZW3D part and generate an adapted 3-view drawing with automatic PMI dimensions. Uses part analysis to choose the base view and layout.",
        "parameters": {
            "type": "object",
            "properties": {}
        }
    },
    {
        "name": "inspect_drawing_views",
        "description": "Inspect the active ZW3D drawing view counts, occupied rectangles, centers, and paper bounds for layout analysis.",
        "parameters": {
            "type": "object",
            "properties": {}
        }
    },
    {
        "name": "evaluate_drawing_quality",
        "description": "Evaluate the active ZW3D drawing against measurable drafting quality rules and return score, rule failures, recommendations, and next optimization action.",
        "parameters": {
            "type": "object",
            "properties": {}
        }
    },
    {
        "name": "generate_optimized_drafting",
        "description": "Generate smart ZW3D drafting, evaluate the resulting drawing against quality rules, and perform basic automatic remediation such as PMI retry when applicable.",
        "parameters": {
            "type": "object",
            "properties": {}
        }
    },
    {
        "name": "generate_3views_with_pmi",
        "description": "Generate three orthographic views (front, top, right) with automatic dimensions in ZW3D CAD. Creates a new drawing sheet from the active 3D part.",
        "parameters": {
            "type": "object",
            "properties": {
                "sheet_name": {
                    "type": "string",
                    "description": "Name of the drawing sheet to create",
                    "default": "Sheet1"
                },
                "scale": {
                    "type": "number",
                    "description": "View scale ratio",
                    "default": 1.0
                }
            }
        }
    },
    {
        "name": "add_pmi_dimensions",
        "description": "Add PMI (Product Manufacturing Information) dimensions to all views in the active ZW3D drawing. Automatically adds dimensions, tolerances, and annotations using ZW3D's built-in auto-dimensioning capabilities.",
        "parameters": {
            "type": "object",
            "properties": {}
        }
    },
    {
        "name": "execute_zw3d_command",
        "description": "Execute a raw ZW3D command string",
        "parameters": {
            "type": "object",
            "properties": {
                "command": {
                    "type": "string",
                    "description": "ZW3D command to execute"
                }
            },
            "required": ["command"]
        }
    },
    {
        "name": "open_zw3d_part",
        "description": "Open a ZW3D part file (.Z3PRT) in the running ZW3D instance. Use this before generating drafting for a specific part.",
        "parameters": {
            "type": "object",
            "properties": {
                "path": {
                    "type": "string",
                    "description": "Absolute path to the .Z3PRT file"
                }
            },
            "required": ["path"]
        }
    },
    {
        "name": "clear_drawing_views",
        "description": "Clear all views on the active ZW3D drawing sheet. Use before regenerating drafting with different parameters.",
        "parameters": {
            "type": "object",
            "properties": {}
        }
    },
    {
        "name": "regenerate_drafting",
        "description": "Regenerate ZW3D drafting with paper size and scale overrides. Use when the initial generation overflows or is too small.",
        "parameters": {
            "type": "object",
            "properties": {
                "paper_index_offset": {
                    "type": "integer",
                    "description": "Offset to add to computed paper preset index (positive = larger paper, e.g. +1 bumps A4 to A3)",
                    "default": 0
                },
                "scale_multiplier": {
                    "type": "number",
                    "description": "Multiplier for computed drawing scale (e.g. 0.9 = 10% smaller, 1.1 = 10% larger)",
                    "default": 1.0
                }
            }
        }
    },
    {
        "name": "add_section_view",
        "description": "Add a full section view to the active ZW3D drawing. Creates a section through the primary base view for showing internal structure.",
        "parameters": {
            "type": "object",
            "properties": {
                "label": {
                    "type": "string",
                    "description": "Section view label (e.g. 'A', 'B')",
                    "default": "A"
                },
                "position": {
                    "type": "string",
                    "description": "Where to place the section view: 'below' or 'right'",
                    "default": "below"
                }
            }
        }
    },
    {
        "name": "run_drafting_agent",
        "description": "Run the iterative drafting agent that analyzes a ZW3D part, generates 3-view drafting with PMI, evaluates quality, and applies corrections in a loop until quality passes. Handles different part types (blocky, plate-like, elongated, stepped) with appropriate strategies.",
        "parameters": {
            "type": "object",
            "properties": {
                "part_path": {
                    "type": "string",
                    "description": "Absolute path to .Z3PRT file. If omitted, uses the currently active part."
                },
                "max_iterations": {
                    "type": "integer",
                    "description": "Maximum agent loop iterations (default 5)",
                    "default": 5
                },
                "pass_score": {
                    "type": "number",
                    "description": "Quality score threshold to pass (default 85, auto-adjusted per profile)",
                    "default": 85.0
                }
            }
        }
    }
]


if __name__ == "__main__":
    # Test the client
    print("Testing ZW3D MCP Client...")
    print(f"Connecting to {ZW3D_BASE_URL}")
    
    # Test generate views
    result = generate_3views_with_pmi("TestSheet", 1.0)
    print("\nGenerate 3 Views Result:")
    print(result)
