# Resume Summary

## Project Title

**ZW3D CAD Automation Bridge and Drafting Agent**

## Short Description

Built a C++/Python automation system for ZW3D 2026 that generates mechanical
drawings through a local HTTP bridge and improves them with an iterative
quality-feedback agent.

## Resume Bullets

- Built a native C++ HTTP plugin for ZW3D 2026 and connected it to Python tools
  for CAD drafting automation.
- Designed an agent loop that analyzes part geometry, generates orthographic
  drawings, evaluates quality rules, and applies corrective actions such as PMI
  insertion, scale adjustment, and section-view generation.
- Implemented reference-driven drafting strategies for blocky, plate-like,
  elongated, and stepped mechanical parts.
- Packaged the project for public GitHub review with mock demos, unit tests, CI,
  and proprietary CAD assets excluded.

## Interview Talking Points

- Why a local HTTP boundary was chosen for CAD integration.
- How the agent maps quality evaluator feedback to corrective actions.
- How part profiles influence paper size, base view, and section-view strategy.
- How to test commercial-CAD automation when CI cannot run the CAD application.
