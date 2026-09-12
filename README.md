# AI Final Project — Emergent Behaviour in Multi-Agent Lane Simulation

This project is a research/demo sandbox that shows how simple, local agent rules can produce complex group behavior. Inspired by lane combat from MOBA games like League of Legends, the simulation models minions, champions, and towers and surfaces higher-level phenomena (freezes, pushes, tower-aggro plays, and more) so observers can explore, tune, and understand emergence in real time.

Why this exists
- To demonstrate and study emergent behaviour that arises from local decision rules and interactions between many independent agents.
- To provide an interactive playground for experimenting with agent tunables and scenarios, and to make emergent phenomena visible, repeatable, and measurable.
- To separate simulation (the canonical ground truth) from observation, enabling non-invasive analysis and visualization of behaviours for teaching, debugging, or research.

Core ideas demonstrated
- Local-first AI: each agent (minion, champion, tower) uses local sensing, steering, and finite-state decision logic. There is no global script that directly orchestrates group tactics.
- Emergence: when many locally-driven agents interact, coherent group-level patterns appear (e.g., wave freezes, slow/fast pushes, tower-aggro plays).
- Determinism & replay: the sim uses a fixed-timestep loop + a seedable xorshift RNG so scenarios replay identically for tuning and evaluation.
- Read-only analysis: an independent Analysis layer samples world state (influence fields, equilibrium point) and detects techniques without modifying simulation state, preserving the ground truth.

What you will find in the code
- Simulation core (Sim.h): flat Entity model, World container, fixed-step Sim_Tick and agent FSMs — the canonical rules that produce emergent behaviour.
- Tunables (Config.h): every timing, stat, and sampling parameter is centralized so experiments are repeatable and easy to iterate.
- Runtime analysis (Analysis.h): influence-field sampling, equilibrium computation, and detectors that name emergent techniques; these drive HUD banners and heatmaps.
- Visualization & Sandbox (Sandbox.h): scenario presets (Neutral, Freeze, SlowPush, Shove, TowerAggro), runtime controls, and ImGui panels for live tuning.
- Integration (main.cpp): ImGui overlay integration and a SwapBuffers hook (MinHook) so the debug UI renders after the engine flush and correctly gates input.
- Renderer interface (Renderer.h): read-only layer used to draw debug overlays (aggro lines, ranges, influence heatmap, attack bars) without changing sim behavior.

How to explore the project (recommended path)
1. Open AI Final Project/AI Final Project/main.cpp to see how the app boots, initializes ImGui, and sets the game state.
2. Inspect src/Config.h to see default tunables and determine which constants to change for experiments.
3. Read src/Sim.h to understand agent rules, FSMs, spawning/waves, and the deterministic stepping logic.
4. Read src/Analysis.h to see how emergent phenomena are detected and how influence/equilibrium are computed.
5. Run the app and use the Sandbox UI to load scenarios, toggle overlays, change the seed, and step the sim to observe deterministic replay.

Controls (default)
- Right mouse button: issue ground/attack-move to the player champion.
- Space: pause / unpause.
- 1: toggle aggro lines
- 2: toggle ranges
- 3: toggle influence heatmap
- 4: toggle wave-equilibrium banner/marker
- 5: toggle attack bars
- . (period) or Right Arrow: single-step while paused
- Q: quit

Build & runtime notes
- Windows-focused, uses Win32 + OpenGL + ImGui. The project expects the CProcessing-style API referenced by cprocessing.h.
- Dependencies: ImGui, MinHook, CProcessing (or the equivalent framework providing CP_* APIs), NanoVG (if not part of the framework), and standard Windows libs (opengl32, gdi32, user32).

