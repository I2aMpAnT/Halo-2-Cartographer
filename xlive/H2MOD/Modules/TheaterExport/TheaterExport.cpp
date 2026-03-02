#include "stdafx.h"
#include "TheaterExport.h"

#include "game/game.h"
#include "game/game_options.h"
#include "game/game_statborg.h"
#include "game/game_time.h"
#include "game/players.h"
#include "networking/logic/life_cycle_manager.h"
#include "networking/Session/network_session.h"
#include "objects/objects.h"
#include "shell/shell.h"
#include "text/unicode.h"
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

// How often to push theater spatial data (in game ticks).
// At 30 tick: divisor 3 = ~10Hz
static const int k_theater_tick_divisor = 3;

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
static bool g_theater_export_registered = false;
static bool g_shutdown_requested = false;
static uint32 g_last_scoreboard_tick = 0;
static uint32 g_last_theater_tick = 0;
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
static void theater_export_post_register(void);
static void theater_export_post_scoreboard(void);
static void theater_export_post_theater_data(void);
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

	// Register with ws_server.py on first in-game tick
	if (!g_theater_export_registered)
	{
		theater_export_post_register();
		g_theater_export_registered = true;
	}

	uint32 current_tick = game_time_get();

	// Scoreboard push at ~3Hz (every k_scoreboard_tick_divisor ticks)
	if (current_tick - g_last_scoreboard_tick >= (uint32)k_scoreboard_tick_divisor)
	{
		theater_export_post_scoreboard();
		g_last_scoreboard_tick = current_tick;
	}

	// Theater spatial data push at ~10Hz (every k_theater_tick_divisor ticks)
	if (current_tick - g_last_theater_tick >= (uint32)k_theater_tick_divisor)
	{
		theater_export_post_theater_data();
		g_last_theater_tick = current_tick;
	}
}

// Called on lifecycle transitions (pre-game, in-game, post-game, etc.)
static void theater_export_lifecycle_callback(e_game_life_cycle state)
{
	if (state == _life_cycle_post_game && g_last_life_cycle == _life_cycle_in_game)
	{
		theater_export_post_game_end();
		g_theater_export_registered = false;
	}

	if (state == _life_cycle_none || state == _life_cycle_pre_game)
	{
		g_theater_export_registered = false;
		g_last_scoreboard_tick = 0;
		g_last_theater_tick = 0;
	}

	g_last_life_cycle = state;
}

// POST /webhook/register - announce this dedi to ws_server.py
static void theater_export_post_register(void)
{
	rapidjson::Document doc;
	doc.SetObject();
	auto& alloc = doc.GetAllocator();

	// Use the dedi server name as the host identifier
	char dedi_name[64];
	_snprintf_s(dedi_name, sizeof(dedi_name), _TRUNCATE, "%s", H2Config_dedi_server_name);

	doc.AddMember("host", rapidjson::Value(dedi_name, alloc), alloc);
	doc.AddMember("port", rapidjson::Value("0"), alloc);  // No overlay WS port; we push via HTTP only
	doc.AddMember("dedi_type", rapidjson::Value("cartographer"), alloc);

	rapidjson::StringBuffer buffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
	doc.Accept(writer);

	theater_export_queue_post("/webhook/register", buffer.GetString());
	event(_event_status, "TheaterExport: Registered with SpartanLounge");
}

// POST /webhook/scoreboard - live scoreboard with team structure
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

	// Build team-based player lists (matches existing ws_server.py scoreboard format)
	rapidjson::Value red_team(rapidjson::kObjectType);
	rapidjson::Value blue_team(rapidjson::kObjectType);
	rapidjson::Value red_players(rapidjson::kArrayType);
	rapidjson::Value blue_players(rapidjson::kArrayType);
	int red_team_score = 0;
	int blue_team_score = 0;

	if (statborg)
	{
		red_team_score = statborg->get_team_stat(0, _statborg_entry_total_score);
		blue_team_score = statborg->get_team_stat(1, _statborg_entry_total_score);
	}

	// Iterate all active players in the session
	c_player_with_unit_iterator player_iterator;
	while (player_iterator.next())
	{
		player_datum* player = player_iterator.get_datum();
		datum player_index = player_iterator.get_index();
		int32 abs_index = player_iterator.get_absolute_index();

		if (!player || player->unit_index == NONE)
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

		// Get unit data for health/shield info
		unit_datum* unit = unit_try_and_get(player->unit_index);

		rapidjson::Value p(rapidjson::kObjectType);
		p.AddMember("name", rapidjson::Value(player_name, alloc), alloc);

		// Stats from statborg
		if (statborg)
		{
			p.AddMember("kills", (int)statborg->get_player_stat(abs_index, _statborg_entry_kills), alloc);
			p.AddMember("deaths", (int)statborg->get_player_stat(abs_index, _statborg_entry_deaths), alloc);
			p.AddMember("assists", (int)statborg->get_player_stat(abs_index, _statborg_entry_assists), alloc);
			p.AddMember("betrayals", (int)statborg->get_player_stat(abs_index, _statborg_entry_betrayals), alloc);
			p.AddMember("suicides", (int)statborg->get_player_stat(abs_index, _statborg_entry_suicides), alloc);
			p.AddMember("score", (int)statborg->get_player_stat(abs_index, _statborg_entry_total_score), alloc);
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

		// Health and shields
		if (unit)
		{
			p.AddMember("alive", (bool)(unit->unit.unit_flags & _unit_is_alive), alloc);
			p.AddMember("shield_vitality", unit->object.shield_vitality, alloc);
			p.AddMember("body_vitality", unit->object.body_vitality, alloc);
		}

		// Add to appropriate team array
		if (team_idx == 0)
			red_players.PushBack(p, alloc);
		else if (team_idx == 1)
			blue_players.PushBack(p, alloc);
		else
		{
			// For FFA or other teams, put in red for now
			red_players.PushBack(p, alloc);
		}
	}

	red_team.AddMember("score", red_team_score, alloc);
	red_team.AddMember("players", red_players, alloc);
	blue_team.AddMember("score", blue_team_score, alloc);
	blue_team.AddMember("players", blue_players, alloc);

	doc.AddMember("red_team", red_team, alloc);
	doc.AddMember("blue_team", blue_team, alloc);

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

// POST /webhook/theater - per-tick player spatial data for 3D theater view
static void theater_export_post_theater_data(void)
{
	if (!game_in_progress() || !game_is_multiplayer())
		return;

	char dedi_name[64];
	_snprintf_s(dedi_name, sizeof(dedi_name), _TRUNCATE, "%s", H2Config_dedi_server_name);

	c_network_session* session = NULL;
	network_life_cycle_in_squad_session(&session);

	rapidjson::Document doc;
	doc.SetObject();
	auto& alloc = doc.GetAllocator();

	doc.AddMember("host", rapidjson::Value(dedi_name, alloc), alloc);
	doc.AddMember("dedi_name", rapidjson::Value(dedi_name, alloc), alloc);
	doc.AddMember("tick", game_time_get(), alloc);

	rapidjson::Value players_arr(rapidjson::kArrayType);

	c_player_with_unit_iterator player_iterator;
	while (player_iterator.next())
	{
		player_datum* player = player_iterator.get_datum();
		datum player_index = player_iterator.get_index();

		if (!player || player->unit_index == NONE)
			continue;

		unit_datum* unit = unit_try_and_get(player->unit_index);
		if (!unit)
			continue;

		// Get player name
		char player_name[64];
		if (session)
		{
			s_membership_player* membership = session->get_player_membership(player_index);
			if (membership && membership->properties_valid)
				wchar_to_utf8(membership->configuration.player_name, player_name, sizeof(player_name));
			else
				_snprintf_s(player_name, sizeof(player_name), _TRUNCATE, "Player");
		}
		else
		{
			_snprintf_s(player_name, sizeof(player_name), _TRUNCATE, "Player");
		}

		rapidjson::Value p(rapidjson::kObjectType);
		p.AddMember("name", rapidjson::Value(player_name, alloc), alloc);

		// Position (world coordinates)
		const _object_datum& obj = unit->object;
		{
			rapidjson::Value pos(rapidjson::kObjectType);
			pos.AddMember("x", obj.position.x, alloc);
			pos.AddMember("y", obj.position.y, alloc);
			pos.AddMember("z", obj.position.z, alloc);
			p.AddMember("position", pos, alloc);
		}

		// Forward vector (orientation)
		{
			rapidjson::Value fwd(rapidjson::kObjectType);
			fwd.AddMember("x", obj.forward.i, alloc);
			fwd.AddMember("y", obj.forward.j, alloc);
			fwd.AddMember("z", obj.forward.k, alloc);
			p.AddMember("forward", fwd, alloc);
		}

		// Translational velocity
		{
			rapidjson::Value vel(rapidjson::kObjectType);
			vel.AddMember("x", obj.translational_velocity.i, alloc);
			vel.AddMember("y", obj.translational_velocity.j, alloc);
			vel.AddMember("z", obj.translational_velocity.k, alloc);
			p.AddMember("velocity", vel, alloc);
		}

		// Aiming vector (where the player is aiming)
		{
			rapidjson::Value aim(rapidjson::kObjectType);
			aim.AddMember("x", unit->unit.aiming_vector.i, alloc);
			aim.AddMember("y", unit->unit.aiming_vector.j, alloc);
			aim.AddMember("z", unit->unit.aiming_vector.k, alloc);
			p.AddMember("aiming", aim, alloc);
		}

		// Looking vector
		{
			rapidjson::Value look(rapidjson::kObjectType);
			look.AddMember("x", unit->unit.looking_vector.i, alloc);
			look.AddMember("y", unit->unit.looking_vector.j, alloc);
			look.AddMember("z", unit->unit.looking_vector.k, alloc);
			p.AddMember("looking", look, alloc);
		}

		// Movement input (throttle)
		{
			rapidjson::Value thr(rapidjson::kObjectType);
			thr.AddMember("x", unit->unit.throttle.i, alloc);
			thr.AddMember("y", unit->unit.throttle.j, alloc);
			thr.AddMember("z", unit->unit.throttle.k, alloc);
			p.AddMember("throttle", thr, alloc);
		}

		// Combat state
		p.AddMember("primary_trigger", unit->unit.primary_trigger, alloc);
		p.AddMember("secondary_trigger", unit->unit.secondary_trigger, alloc);
		p.AddMember("zoom_level", (int)unit->unit.zoom_level, alloc);
		p.AddMember("crouching", unit->unit.crouching, alloc);
		p.AddMember("active_camo_power", unit->unit.active_camo_power, alloc);

		// Health/shields
		p.AddMember("shield_vitality", obj.shield_vitality, alloc);
		p.AddMember("body_vitality", obj.body_vitality, alloc);
		p.AddMember("shield_damage", obj.current_shield_damage, alloc);
		p.AddMember("body_damage", obj.current_body_damage, alloc);

		// Team
		p.AddMember("team", (int)unit->unit.unit_team, alloc);

		// Grenade counts
		{
			rapidjson::Value grenades(rapidjson::kArrayType);
			for (int g = 0; g < k_unit_grenade_types_count; g++)
			{
				grenades.PushBack((int)unit->unit.grenade_counts[g], alloc);
			}
			p.AddMember("grenades", grenades, alloc);
		}

		// Alive state
		p.AddMember("alive", (bool)(unit->unit.unit_flags & _unit_is_alive), alloc);

		players_arr.PushBack(p, alloc);
	}

	doc.AddMember("players", players_arr, alloc);

	rapidjson::StringBuffer buffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
	doc.Accept(writer);

	theater_export_queue_post("/webhook/theater", buffer.GetString());
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
