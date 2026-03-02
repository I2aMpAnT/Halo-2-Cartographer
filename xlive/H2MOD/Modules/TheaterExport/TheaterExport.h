#pragma once

/*
 * TheaterExport Module
 *
 * Exports real-time game state from the H2 Cartographer dedicated server
 * to the SpartanLounge WebSocket stats server (ws_server.py:9090).
 *
 * Uses the existing HTTP webhook protocol that ws_server.py already supports:
 *   POST /webhook/register    - Announce dedi on startup
 *   POST /webhook/scoreboard  - Push live scoreboard (~3Hz from game loop)
 *   POST /webhook/killfeed    - Push kill events
 *   POST /webhook/game        - Push game-end notification
 *
 * Additionally pushes per-tick player spatial data (position, orientation,
 * aiming, velocity) via a new webhook endpoint that SpartanLounge can add:
 *   POST /webhook/theater     - Per-tick player positions for 3D theater view
 *
 * Architecture:
 *   game_loop callback (execute_after)
 *     -> collect player data via c_player_in_game_iterator + unit_datum
 *     -> serialize to JSON via RapidJSON (already bundled)
 *     -> HTTP POST via libcurl (already bundled) on a background thread
 *
 * Only active on dedicated servers (shell_is_dedicated_server()).
 * Configurable via H2Config: target host/port and tick rate divisor.
 */

namespace TheaterExport
{
	// Initialize the module: register game loop + lifecycle callbacks.
	// Called from H2MOD::Initialize() for dedi servers only.
	void Initialize(void);

	// Tear down: cancel background thread, clean up curl handles.
	void Dispose(void);
}
