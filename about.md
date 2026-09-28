# Real Pathfinder

Finds a route through a classic level **using Geometry Dash's own physics**, so every object works:
2.2 objects, triggers, dash orbs, D/J/S blocks, upside-down slopes, teleports, robot, spider, swing and duals.

How it works: the level runs hidden, one real 240 TPS physics tick at a time. The search tries button
timings where the player dies, keeps the branch that gets furthest, and backtracks out of dead ends.
Found routes are replayed from 0% without any checkpoints before they are saved, and any spot that
fails that clean replay is searched again in exact mode.

## Use
1. Open a level (normal mode, from 0%, no Start Pos).
2. Pause > **Pathfinder** > **Start**.
3. Press Esc to check progress or stop. The macro is saved to the mod's save folder (`macros`).

Output: `.json` for the Frame Window Analyzer mod, and `.gdr.json` for GDR-compatible bots.

Not supported: platformer levels.
