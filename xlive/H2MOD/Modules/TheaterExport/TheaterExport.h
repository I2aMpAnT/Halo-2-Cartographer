#pragma once

/*
 * TheaterExport Module
 *
 * Exports real-time game state from the H2 Cartographer dedicated server
 * to the SpartanLounge WebSocket stats server (ws_server.py:9090).
 *
 * Uses the existing HTTP webhook protocol that ws_server.py already supports:
 *   POST /webhook/scoreboard  - Push live scoreboard (~3Hz from game loop)
 *   POST /webhook/game        - Push game-end notification
 *
 * Auto-registers with ws_server.py on first scoreboard push (no explicit
 * register needed - ws_server.py's _auto_register handles it).
 *
 * Architecture:
 *   game_loop callback (execute_after)
 *     -> collect player data via c_player_with_unit_iterator + statborg
 *     -> serialize to JSON via RapidJSON (already bundled)
 *     -> HTTP POST via libcurl (already bundled) on a background thread
 *
 * Only active on dedicated servers (shell_is_dedicated_server()).
 */

namespace TheaterExport
{
	// Initialize the module: register game loop + lifecycle callbacks.
	// Called from H2MOD::Initialize() for dedi servers only.
	void Initialize(void);

	// Tear down: cancel background thread, clean up curl handles.
	void Dispose(void);
}
