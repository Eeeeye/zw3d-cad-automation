# Case Study: ZW3D CAD Drafting Automation

## Background

ZW3D can generate drawings from 3D parts, but production-quality drafting still
requires repeated manual decisions: base view selection, projected view layout,
paper size, PMI placement, section views, and quality inspection. The goal of
this project was to explore whether those decisions could be coordinated by an
agent loop instead of a one-shot script.

## Problem

Simple macro automation was not enough. Different part types need different
drafting behavior:

- Thin plate-like parts often need larger horizontal sheets and section views.
- Blocky parts can fit on smaller sheets with a standard three-view layout.
- Stepped parts may need secondary base views or section views.
- PMI can improve completeness but can also introduce clutter or overlaps.

The automation needed to observe the current CAD state, choose a strategy, and
react to measurable quality feedback.

## Solution

The project is split into three layers:

1. **ZW3D C++ plugin**  
   A native DLL is loaded by ZW3D and exposes CAD operations through local HTTP
   endpoints.

2. **Python bridge client**  
   A retry-aware client wraps the HTTP API and normalizes operations such as
   `analyze_part`, `generate_smart_drafting`, `add_pmi`, and
   `evaluate_drawing_quality`.

3. **Drafting agent**  
   The agent runs a closed loop:

   ```text
   analyze part -> select strategy -> generate drawing -> evaluate quality
   -> apply corrective action -> repeat or accept
   ```

## Engineering Decisions

- Use local HTTP instead of process automation so Python tools and MCP-style
  wrappers can communicate with ZW3D through a stable boundary.
- Keep ZW3D as the source of truth for CAD operations instead of trying to
  replicate CAD geometry logic in Python.
- Encode drafting strategy separately from the agent loop so part-type behavior
  can evolve without rewriting orchestration code.
- Provide a mock demo and unit tests because public CI cannot run commercial
  ZW3D binaries.
- Exclude proprietary CAD samples, compiled DLLs, and generated submissions from
  the public repository.

## Result

The repository demonstrates:

- A C++/Python integration boundary for CAD automation.
- An agentic drafting loop with measurable feedback.
- Strategy mapping for several mechanical part profiles.
- A reviewable public package suitable for portfolio and interview discussion.

## My Contribution

- Designed the local ZW3D bridge architecture.
- Implemented the Python client and agent loop.
- Defined strategy and corrective-action mappings.
- Built reference-profile learning/verification utilities.
- Packaged the project for GitHub with public-safe assets and documentation.

## What I Would Improve Next

- Add a real demo video from a ZW3D run.
- Add image-based drawing comparison metrics.
- Build a small desktop dashboard for running the agent and inspecting quality
  history.
- Expand the strategy set with more part families and more reference drawings.
