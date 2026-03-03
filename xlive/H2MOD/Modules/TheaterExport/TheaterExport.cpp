#include "stdafx.h"
#include "TheaterExport.h"

#include "game/game.h"
#include "game/game_options.h"
#include "game/game_statborg.h"
#include "game/game_time.h"
#include "game/players.h"
#include "networking/logic/life_cycle_manager.h"
#include "networking/Session/network_session.h"
#include "shell/shell.h"
#include "units/units.h"

#include "H2MOD/Modules/EventHandler/EventHandler.hpp"
#include "H2MOD/Modules/Shell/Config.h"

#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"

#include "curl/curl.h"

/* constants */

// SpartanLounge ws_server.py webhook endpoints
// Default target: 10.10.10.2:9090 (SpartanLounge container, internal network)
static const char* k_theater_export_host = "10.10.10.2";
static const int   k_theater_export_port = 9090;

// How often to push scoreboard data (in game ticks).
// At 30 tick: divisor 10 = ~3Hz, divisor 5 = ~6Hz
static const int k_scoreboard_tick_divisor = 10;

// Game engine type names (matches e_game_engine_type)
static const char* k_game_engine_names[] = {
	"None",
	"CTF",
	"Slayer",
	"Oddball",
	"King of the Hill",
	"Race",
	"Headhunter",
	"Juggernaut",
	"Territories",
	"Assault",
	"Stub"
};

// Team names
static const char* k_team_names[] = {
	"Red", "Blue", "Yellow", "Green",
	"Purple", "Orange", "Brown", "Pink"
};

/* globals */

static bool g_theater_export_initialized = false;
static bool g_shutdown_requested = false;
static uint32 g_last_scoreboard_tick = 0;
static e_game_life_cycle g_last_life_cycle = _life_cycle_none;

// Background thread for HTTP posts (so we don't block the game loop)
static HANDLE g_post_thread = NULL;
static CRITICAL_SECTION g_post_lock;

// Queued JSON payloads to send
struct s_post_request
{
	std::string endpoint;
	std::string json_body;
};

static std::vector<s_post_request> g_post_queue;

/* prototypes */

static void theater_export_game_loop_callback(void);
static void theater_export_lifecycle_callback(e_game_life_cycle state);
static void theater_export_post_scoreboard(void);
static void theater_export_post_game_end(void);
static void theater_export_queue_post(const char* endpoint, const char* json);
static DWORD WINAPI theater_export_post_thread(LPVOID param);
static size_t theater_export_curl_write_callback(void* contents, size_t size, size_t nmemb, void* userp);

/* helpers */

static void wchar_to_utf8(const wchar_t* src, char* dst, int dst_size)
{
	if (!src || !dst || dst_size <= 0)
	{
		if (dst && dst_size > 0) dst[0] = '\0';
		return;
	}
	WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, dst_size, NULL, NULL);
	dst[dst_size - 1] = '\0';
}

static const char* get_engine_name(e_game_engine_type engine)
{
	if (engine >= 0 && engine < NUMBEROF(k_game_engine_names))
		return k_game_engine_names[engine];
	return "Unknown";
}

static const char* get_team_name(int8 team_index)
{
	if (team_index >= 0 && team_index < NUMBEROF(k_team_names))
		return k_team_names[team_index];
	return "Unknown";
}

static void build_url(char* out, size_t out_size, const char* endpoint)
{
	_snprintf_s(out, out_size, _TRUNCATE, "http://%s:%d%s", k_theater_export_host, k_theater_export_port, endpoint);
}

/* public code */

void TheaterExport::Initialize(void)
{
	if (!shell_is_dedicated_server())
		return;

	InitializeCriticalSection(&g_post_lock);

	// Start the background HTTP post thread
	g_shutdown_requested = false;
	g_post_thread = CreateThread(NULL, 0, theater_export_post_thread, NULL, 0, NULL);

	// Register game loop callback (fires every tick on the dedi)
	EventHandler::register_callback(
		(void*)theater_export_game_loop_callback,
		EventType::game_loop,
		EventExecutionType::execute_after
	);

	// Register lifecycle callback (fires on state transitions)
	EventHandler::register_callback(
		(void*)theater_export_lifecycle_callback,
		EventType::gamelifecycle_change,
		EventExecutionType::execute_after
	);

	g_theater_export_initialized = true;
	event(_event_status, "TheaterExport: Initialized");
}

void TheaterExport::Dispose(void)
{
	if (!g_theater_export_initialized)
		return;

	g_shutdown_requested = true;

	if (g_post_thread)
	{
		WaitForSingleObject(g_post_thread, 5000);
		CloseHandle(g_post_thread);
		g_post_thread = NULL;
	}

	DeleteCriticalSection(&g_post_lock);
	g_theater_export_initialized = false;
}

/* private code */

// Called every game tick on the dedi server (after game loop update)
static void theater_export_game_loop_callback(void)
{
	if (!game_in_progress() || !game_is_multiplayer())
		return;

	e_game_life_cycle life_cycle = get_game_life_cycle();
	if (life_cycle != _life_cycle_in_game)
		return;

	uint32 current_tick = game_time_get();

	// Scoreboard push at ~3Hz (every k_scoreboard_tick_divisor ticks)
	if (current_tick - g_last_scoreboard_tick >= (uint32)k_scoreboard_tick_divisor)
	{
		theater_export_post_scoreboard();
		g_last_scoreboard_tick = current_tick;
	}
}

// Called on lifecycle transitions (pre-game, in-game, post-game, etc.)
static void theater_export_lifecycle_callback(e_game_life_cycle state)
{
	if (state == _life_cycle_post_game && g_last_life_cycle == _life_cycle_in_game)
	{
		theater_export_post_game_end();
	}

	if (state == _life_cycle_none || state == _life_cycle_pre_game)
	{
		g_last_scoreboard_tick = 0;
	}

	g_last_life_cycle = state;
}

// POST /webhook/scoreboard - live scoreboard with team structure
// First scoreboard push auto-registers the dedi with ws_server.py
static void theater_export_post_scoreboard(void)
{
	c_network_session* session = NULL;
	if (!network_life_cycle_in_squad_session(&session))
		return;

	s_game_options* options = game_options_get();
	if (!options)
		return;

	c_game_statborg* statborg = game_engine_get_statborg();

	rapidjson::Document doc;
	doc.SetObject();
	auto& alloc = doc.GetAllocator();

	// Server metadata
	char dedi_name[64];
	_snprintf_s(dedi_name, sizeof(dedi_name), _TRUNCATE, "%s", H2Config_dedi_server_name);
	doc.AddMember("host", rapidjson::Value(dedi_name, alloc), alloc);
	doc.AddMember("dedi_name", rapidjson::Value(dedi_name, alloc), alloc);

	// Map name from scenario path
	char map_name[MAX_PATH];
	wchar_to_utf8(options->scenario_path, map_name, sizeof(map_name));
	// Extract just the map name from the full path
	const char* last_slash = strrchr(map_name, '\\');
	const char* map_short = last_slash ? last_slash + 1 : map_name;
	doc.AddMember("map_name", rapidjson::Value(map_short, alloc), alloc);

	// Game variant info
	s_game_variant* variant = &options->game_variant;
	char variant_name[128];
	wchar_to_utf8(variant->variant_name, variant_name, sizeof(variant_name));
	doc.AddMember("variant_name", rapidjson::Value(variant_name, alloc), alloc);

	const char* game_type = get_engine_name(variant->variant_game_engine_index);
	doc.AddMember("game_type", rapidjson::Value(game_type, alloc), alloc);

	// Lifecycle
	e_game_life_cycle life_cycle = get_game_life_cycle();
	doc.AddMember("life_cycle", (int)life_cycle, alloc);

	const char* life_cycle_names[] = { "None", "PreGame", "StartGame", "InGame", "PostGame", "Joining", "Matchmaking" };
	const char* lc_name = (life_cycle >= 0 && life_cycle < NUMBEROF(life_cycle_names)) ? life_cycle_names[life_cycle] : "Unknown";
	doc.AddMember("life_cycle_name", rapidjson::Value(lc_name, alloc), alloc);
	doc.AddMember("in_game", life_cycle == _life_cycle_in_game, alloc);

	// Is team play?
	bool team_play = TEST_BIT(variant->game_engine_flags, _game_engine_teams_bit);
	doc.AddMember("team_play", team_play, alloc);

	// Team count
	doc.AddMember("team_count", variant->maximum_allowable_teams, alloc);

	// Team scores
	rapidjson::Value team_scores(rapidjson::kArrayType);
	if (statborg)
	{
		for (int t = 0; t < (int)variant->maximum_allowable_teams; t++)
		{
			rapidjson::Value ts(rapidjson::kObjectType);
			ts.AddMember("team", t, alloc);
			ts.AddMember("team_name", rapidjson::Value(get_team_name((int8)t), alloc), alloc);
			ts.AddMember("score", (int)statborg->get_team_stat(t, _statborg_entry_total_score), alloc);
			team_scores.PushBack(ts, alloc);
		}
	}
	doc.AddMember("team_scores", team_scores, alloc);

	// Flat players array (matches HaloCaster format)
	rapidjson::Value players(rapidjson::kArrayType);

	c_player_in_game_iterator player_iterator;
	while (player_iterator.next())
	{
		player_datum* player = player_iterator.get_datum();
		datum player_index = player_iterator.get_index();
		int32 abs_index = player_iterator.get_absolute_index();

		if (!player)
			continue;

		// Get player name
		char player_name[64];
		s_membership_player* membership = session->get_player_membership(player_index);
		if (membership && membership->properties_valid)
		{
			wchar_to_utf8(membership->configuration.player_name, player_name, sizeof(player_name));
		}
		else
		{
			_snprintf_s(player_name, sizeof(player_name), _TRUNCATE, "Player_%d", abs_index);
		}

		// Get unit data (may be NULL if player is dead / hasn't spawned)
		bool has_unit = (player->unit_index != NONE);
		unit_datum* unit = has_unit ? unit_try_and_get(player->unit_index) : NULL;

		rapidjson::Value p(rapidjson::kObjectType);
		p.AddMember("name", rapidjson::Value(player_name, alloc), alloc);
		p.AddMember("index", abs_index, alloc);

		// Stats from statborg
		int kills = 0, deaths = 0, assists = 0;
		if (statborg)
		{
			kills = (int)statborg->get_player_stat(abs_index, _statborg_entry_kills);
			deaths = (int)statborg->get_player_stat(abs_index, _statborg_entry_deaths);
			assists = (int)statborg->get_player_stat(abs_index, _statborg_entry_assists);
			p.AddMember("kills", kills, alloc);
			p.AddMember("deaths", deaths, alloc);
			p.AddMember("assists", assists, alloc);
			p.AddMember("betrayals", (int)statborg->get_player_stat(abs_index, _statborg_entry_betrayals), alloc);
			p.AddMember("suicides", (int)statborg->get_player_stat(abs_index, _statborg_entry_suicides), alloc);
			p.AddMember("score", (int)statborg->get_player_stat(abs_index, _statborg_entry_total_score), alloc);

			// K/D ratio (HaloCaster format: deaths=0 returns kills as ratio)
			double kd = (deaths > 0) ? (double)kills / (double)deaths : (double)kills;
			p.AddMember("kd_ratio", kd, alloc);
		}

		// Team info
		int8 team_idx = -1;
		if (membership && membership->properties_valid)
		{
			team_idx = membership->configuration.team_index;
		}
		else if (unit)
		{
			team_idx = (int8)unit->unit.unit_team;
		}
		p.AddMember("team", (int)team_idx, alloc);
		p.AddMember("team_name", rapidjson::Value(get_team_name(team_idx), alloc), alloc);

		// Alive / dead / quit flags
		bool alive = unit && (unit->unit.unit_flags & _unit_is_alive);
		p.AddMember("alive", alive, alloc);
		p.AddMember("is_dead", !alive, alloc);
		p.AddMember("is_quit", false, alloc); // TODO: track quit state

		// Positional + spatial data (only meaningful when unit exists)
		if (unit)
		{
			// Position (world coordinates, 4 decimal precision in JSON)
			rapidjson::Value pos(rapidjson::kObjectType);
			pos.AddMember("x", unit->object.position.x, alloc);
			pos.AddMember("y", unit->object.position.y, alloc);
			pos.AddMember("z", unit->object.position.z, alloc);
			p.AddMember("position", pos, alloc);

			// Yaw/Pitch from aiming_vector (i, j, k components)
			// aiming_vector: i = forward X, j = forward Y, k = vertical component
			real_vector3d& aim = unit->unit.aiming_vector;
			float yaw_rad = atan2f(aim.j, aim.i);
			float pitch_rad = asinf(fmaxf(-0.999f, fminf(0.999f, aim.k)));
			float yaw_deg = yaw_rad * (180.0f / 3.14159265f);
			float pitch_deg = pitch_rad * (180.0f / 3.14159265f);

			// Normalize yaw to 0-360
			if (yaw_deg < 0.0f) yaw_deg += 360.0f;

			p.AddMember("yaw_rad", yaw_rad, alloc);
			p.AddMember("pitch_rad", pitch_rad, alloc);
			p.AddMember("yaw_deg", yaw_deg, alloc);
			p.AddMember("pitch_deg", pitch_deg, alloc);

			// Crouch state (crouching float > 0.5 means crouched)
			p.AddMember("crouching", unit->unit.crouching > 0.5f, alloc);

			// Airborne - check if player has vertical velocity (no explicit airborne ticks field exposed)
			bool airborne = fabsf(unit->object.translational_velocity.k) > 0.01f;
			p.AddMember("airborne", airborne, alloc);

			// Shield and body vitality
			p.AddMember("shield_vitality", unit->object.shield_vitality, alloc);
			p.AddMember("body_vitality", unit->object.body_vitality, alloc);
		}
		else
		{
			// Player exists but has no unit (dead, not yet spawned)
			rapidjson::Value pos(rapidjson::kObjectType);
			pos.AddMember("x", 0.0, alloc);
			pos.AddMember("y", 0.0, alloc);
			pos.AddMember("z", 0.0, alloc);
			p.AddMember("position", pos, alloc);
			p.AddMember("yaw_rad", 0.0, alloc);
			p.AddMember("pitch_rad", 0.0, alloc);
			p.AddMember("yaw_deg", 0.0, alloc);
			p.AddMember("pitch_deg", 0.0, alloc);
			p.AddMember("crouching", false, alloc);
			p.AddMember("airborne", false, alloc);
			p.AddMember("shield_vitality", 0.0, alloc);
			p.AddMember("body_vitality", 0.0, alloc);
		}

		players.PushBack(p, alloc);
	}

	doc.AddMember("players", players, alloc);

	// Game duration (ticks -> seconds)
	uint32 game_ticks = game_time_get();
	int32 tick_rate = game_tick_rate();
	int duration_seconds = (tick_rate > 0) ? (int)(game_ticks / tick_rate) : 0;
	doc.AddMember("duration_seconds", duration_seconds, alloc);

	rapidjson::StringBuffer buffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
	doc.Accept(writer);

	theater_export_queue_post("/webhook/scoreboard", buffer.GetString());
}

// POST /webhook/game - game end notification
static void theater_export_post_game_end(void)
{
	rapidjson::Document doc;
	doc.SetObject();
	auto& alloc = doc.GetAllocator();

	char dedi_name[64];
	_snprintf_s(dedi_name, sizeof(dedi_name), _TRUNCATE, "%s", H2Config_dedi_server_name);
	doc.AddMember("host", rapidjson::Value(dedi_name, alloc), alloc);
	doc.AddMember("dedi_name", rapidjson::Value(dedi_name, alloc), alloc);

	rapidjson::StringBuffer buffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
	doc.Accept(writer);

	theater_export_queue_post("/webhook/game", buffer.GetString());
	event(_event_status, "TheaterExport: Game end sent");
}

// Queue a JSON POST for the background thread
static void theater_export_queue_post(const char* endpoint, const char* json)
{
	EnterCriticalSection(&g_post_lock);
	g_post_queue.push_back({ endpoint, json });
	LeaveCriticalSection(&g_post_lock);
}

// Background thread that drains the post queue and sends HTTP requests
static DWORD WINAPI theater_export_post_thread(LPVOID param)
{
	UNREFERENCED_PARAMETER(param);

	while (!g_shutdown_requested)
	{
		// Drain the queue
		std::vector<s_post_request> pending;
		EnterCriticalSection(&g_post_lock);
		pending.swap(g_post_queue);
		LeaveCriticalSection(&g_post_lock);

		for (const auto& req : pending)
		{
			char url[256];
			build_url(url, sizeof(url), req.endpoint.c_str());

			CURL* curl = curl_interface_init_no_verify();
			if (!curl)
				continue;

			struct curl_slist* headers = NULL;
			headers = curl_slist_append(headers, "Content-Type: application/json");

			curl_easy_setopt(curl, CURLOPT_URL, url);
			curl_easy_setopt(curl, CURLOPT_POST, 1L);
			curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.json_body.c_str());
			curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)req.json_body.size());
			curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
			curl_easy_setopt(curl, CURLOPT_TIMEOUT, 2L);
			curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 2L);
			curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, theater_export_curl_write_callback);
			curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

			CURLcode res = curl_easy_perform(curl);
			if (res != CURLE_OK)
			{
				// Silently drop - don't spam logs for network issues
				// The dedi must keep running regardless of theater export status
			}

			curl_slist_free_all(headers);
			curl_easy_cleanup(curl);
		}

		// Sleep briefly to avoid busy-spinning when idle
		Sleep(15);
	}

	return 0;
}

// Discard curl response body (we don't need the response)
static size_t theater_export_curl_write_callback(void* contents, size_t size, size_t nmemb, void* userp)
{
	UNREFERENCED_PARAMETER(contents);
	UNREFERENCED_PARAMETER(userp);
	return size * nmemb;
}
