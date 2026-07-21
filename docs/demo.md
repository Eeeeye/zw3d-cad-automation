# Demo Guide

This project has two demo paths: a public mock demo that runs anywhere, and a
full CAD demo that requires ZW3D 2026.

## 1. Mock Demo

Run:

```powershell
python examples\mock_agent_demo.py
```

The demo uses a fake ZW3D client with scripted responses:

1. `check_server` succeeds.
2. `analyze_part` returns a plate-like part.
3. `generate_smart_drafting` creates an initial drawing.
4. `evaluate_quality` reports missing PMI.
5. The agent calls `add_pmi`.
6. A second `evaluate_quality` accepts the result.

This path is intended for recruiters, interviewers, and CI environments that do
not have ZW3D installed.

## 2. Full ZW3D Demo

Prerequisites:

- ZW3D 2026
- Visual Studio 2022 C++ Build Tools
- Python dependencies installed from `requirements.txt`

Build the plugin:

```powershell
cd ZW3D_Bridge
.\compile_ps.ps1
```

Restart ZW3D, load a part, then run:

```powershell
python ZW3D_Agent\run_agent.py --active --wait --verbose
```

For a local part file:

```powershell
python ZW3D_Agent\run_agent.py --part "C:\path\to\part.Z3PRT" --wait --verbose
```

## Recommended Portfolio Recording

For a polished project showcase, record:

1. ZW3D with the part open.
2. The terminal running `run_agent.py`.
3. The generated drawing view layout.
4. The final quality score and iteration history.

Suggested assets:

- `assets/demo.gif` - 20 to 40 seconds, terminal plus ZW3D result.
- `assets/result-before-after.png` - initial drawing vs. corrected drawing.
- `assets/quality-history.png` - iteration score improvement.

Binary demo captures are intentionally not committed yet because this public
package avoids proprietary CAD files and generated images.
