# ZW3D CAD Automation Bridge

Automation toolkit for ZW3D 2026. The project exposes a local HTTP bridge from a
ZW3D C++ plugin, provides Python helpers/MCP-style tool wrappers, and includes an
iterative drafting agent for generating and evaluating orthographic drawings with
PMI.

## What Is Included

- `ZW3D_Bridge/` - C++ HTTP server plugin source, build scripts, and Python bridge client.
- `ZW3D_Agent/` - Reference-driven drafting agent and quality iteration logic.
- `ZW3D_Automation/` - PowerShell, batch, and Python automation entry points.
- `docs/` - Notes from reference drawing analysis and interface extraction.
- `test_*.py`, `test_*.ps1`, `verify_workflow.py` - Smoke tests and workflow checks.

Generated reports, personal submission files, screenshots, Python caches, compiled
objects, and DLL binaries are intentionally excluded from this release package.

## Requirements

- Windows
- ZW3D 2026 installed at `C:\Program Files\ZWSOFT\ZW3D 2026`
- Visual Studio 2022 C++ Build Tools
- Python 3.10+
- Python package dependencies from `requirements.txt`

Install Python dependencies:

```powershell
python -m pip install -r requirements.txt
```

## Build The ZW3D Plugin

```powershell
cd ZW3D_Bridge
.\compile_ps.ps1
```

The build script creates `ZW3D_Bridge\bin\ZW3D_HTTP_Server.dll` and, when the
ZW3D custom API folder exists, copies it to:

```text
%APPDATA%\ZWSOFT\ZW3D\ZW3D2026\custom\apilibs\
```

Restart ZW3D after installing the DLL so the plugin can be loaded.

## Run The Drafting Agent

Start ZW3D with the bridge plugin loaded, then run:

```powershell
python ZW3D_Agent\run_agent.py --active --wait
```

To run against a specific part file:

```powershell
python ZW3D_Agent\run_agent.py --part "C:\path\to\part.Z3PRT" --wait
```

## HTTP Bridge

The bridge listens on `localhost:8081` and includes endpoints for status checks,
part analysis, view inspection, smart drafting generation, PMI creation, drawing
quality evaluation, and command execution.

Useful checks:

```powershell
curl http://localhost:8081/status
curl http://localhost:8081/analyze_part
```

## Samples

The original local workspace used proprietary ZW3D sample drawings and parts for
reference learning. Those CAD binaries are not included in this GitHub package.
Place your own `.Z3PRT` and `.Z3DRW` files under `samples/` or pass absolute paths
to the command-line tools.

## License

No open-source license has been selected yet. Add a license before accepting
external contributions or redistribution.
