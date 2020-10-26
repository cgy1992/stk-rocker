//
//  SuperTuxKart - a fun racing game with go-kart
//  Copyright (C) 2013-2015 SuperTuxKart-Team
//
//  This program is free software; you can redistribute it and/or
//  modify it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; either version 3
//  of the License, or (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, write to the Free Software
//  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

#include "network/protocols/server_lobby.hpp"

#include "addons/addon.hpp"
#include "config/user_config.hpp"
#include "items/network_item_manager.hpp"
#include "items/powerup_manager.hpp"
#include "karts/abstract_kart.hpp"
#include "karts/controller/player_controller.hpp"
#include "karts/kart_properties.hpp"
#include "karts/kart_properties_manager.hpp"
#include "modes/capture_the_flag.hpp"
#include "modes/soccer_world.hpp"
#include "modes/linear_world.hpp"
#include "network/crypto.hpp"
#include "network/event.hpp"
#include "network/game_setup.hpp"
#include "network/network.hpp"
#include "network/network_config.hpp"
#include "network/network_player_profile.hpp"
#include "network/peer_vote.hpp"
#include "network/protocol_manager.hpp"
#include "network/protocols/connect_to_peer.hpp"
#include "network/protocols/game_protocol.hpp"
#include "network/protocols/game_events_protocol.hpp"
#include "network/race_event_manager.hpp"
#include "network/server_config.hpp"
#include "network/socket_address.hpp"
#include "network/stk_host.hpp"
#include "network/stk_ipv6.hpp"
#include "network/stk_peer.hpp"
#include "online/online_profile.hpp"
#include "online/request_manager.hpp"
#include "online/xml_request.hpp"
#include "race/race_manager.hpp"
#include "tracks/check_manager.hpp"
#include "tracks/track.hpp"
#include "tracks/track_manager.hpp"
#include "utils/log.hpp"
#include "utils/random_generator.hpp"
#include "utils/string_utils.hpp"
#include "utils/time.hpp"
#include "utils/translation.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <iterator>

// We use max priority for all server requests to avoid downloading of addons
// icons blocking the poll request in all-in-one graphical client server

#ifdef ENABLE_SQLITE3

// ----------------------------------------------------------------------------
static void upperIPv6SQL(sqlite3_context* context, int argc,
                         sqlite3_value** argv)
{
    if (argc != 1)
    {
        sqlite3_result_int64(context, 0);
        return;
    }

    char* ipv6 = (char*)sqlite3_value_text(argv[0]);
    if (ipv6 == NULL)
    {
        sqlite3_result_int64(context, 0);
        return;
    }
    sqlite3_result_int64(context, upperIPv6(ipv6));
}

// ----------------------------------------------------------------------------
void insideIPv6CIDRSQL(sqlite3_context* context, int argc,
                       sqlite3_value** argv)
{
    if (argc != 2)
    {
        sqlite3_result_int(context, 0);
        return;
    }

    char* ipv6_cidr = (char*)sqlite3_value_text(argv[0]);
    char* ipv6_in = (char*)sqlite3_value_text(argv[1]);
    if (ipv6_cidr == NULL || ipv6_in == NULL)
    {
        sqlite3_result_int(context, 0);
        return;
    }
    sqlite3_result_int(context, insideIPv6CIDR(ipv6_cidr, ipv6_in));
}   // insideIPv6CIDRSQL

// ----------------------------------------------------------------------------
/*
Copy below code so it can be use as loadable extension to be used in sqlite3
command interface (together with andIPv6 and insideIPv6CIDR from stk_ipv6)

#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT1
// ----------------------------------------------------------------------------
sqlite3_extension_init(sqlite3* db, char** pzErrMsg,
                       const sqlite3_api_routines* pApi)
{
    SQLITE_EXTENSION_INIT2(pApi)
    sqlite3_create_function(db, "insideIPv6CIDR", 2, SQLITE_UTF8, NULL,
        insideIPv6CIDRSQL, NULL, NULL);
    sqlite3_create_function(db, "upperIPv6", 1, SQLITE_UTF8,  0, upperIPv6SQL,
        0, 0);
    return 0;
}   // sqlite3_extension_init
*/

#endif

/** This is the central game setup protocol running in the server. It is
 *  mostly a finite state machine. Note that all nodes in ellipses and light
 *  grey background are actual states; nodes in boxes and white background 
 *  are functions triggered from a state or triggering potentially a state
 *  change.
 \dot
 digraph interaction {
 node [shape=box]; "Server Constructor"; "playerTrackVote"; "connectionRequested"; 
                   "signalRaceStartToClients"; "startedRaceOnClient"; "loadWorld";
 node [shape=ellipse,style=filled,color=lightgrey];

 "Server Constructor" -> "INIT_WAN" [label="If WAN game"]
 "Server Constructor" -> "WAITING_FOR_START_GAME" [label="If LAN game"]
 "INIT_WAN" -> "GETTING_PUBLIC_ADDRESS" [label="GetPublicAddress protocol callback"]
 "GETTING_PUBLIC_ADDRESS" -> "WAITING_FOR_START_GAME" [label="Register server"]
 "WAITING_FOR_START_GAME" -> "connectionRequested" [label="Client connection request"]
 "connectionRequested" -> "WAITING_FOR_START_GAME"
 "WAITING_FOR_START_GAME" -> "SELECTING" [label="Start race from authorised client"]
 "SELECTING" -> "SELECTING" [label="Client selects kart, #laps, ..."]
 "SELECTING" -> "playerTrackVote" [label="Client selected track"]
 "playerTrackVote" -> "SELECTING" [label="Not all clients have selected"]
 "playerTrackVote" -> "LOAD_WORLD" [label="All clients have selected; signal load_world to clients"]
 "LOAD_WORLD" -> "loadWorld"
 "loadWorld" -> "WAIT_FOR_WORLD_LOADED" 
 "WAIT_FOR_WORLD_LOADED" -> "WAIT_FOR_WORLD_LOADED" [label="Client or server loaded world"]
 "WAIT_FOR_WORLD_LOADED" -> "signalRaceStartToClients" [label="All clients and server ready"]
 "signalRaceStartToClients" -> "WAIT_FOR_RACE_STARTED"
 "WAIT_FOR_RACE_STARTED" ->  "startedRaceOnClient" [label="Client has started race"]
 "startedRaceOnClient" -> "WAIT_FOR_RACE_STARTED" [label="Not all clients have started"]
 "startedRaceOnClient" -> "DELAY_SERVER" [label="All clients have started"]
 "DELAY_SERVER" -> "DELAY_SERVER" [label="Not done waiting"]
 "DELAY_SERVER" -> "RACING" [label="Server starts race now"]
 }
 \enddot


 *  It starts with detecting the public ip address and port of this
 *  host (GetPublicAddress).
 */
ServerLobby::ServerLobby() : LobbyProtocol()
{
	//ServerConfig::m_race_tournament = true;
	//ServerConfig::m_race_tournament_players = "P TheRocker Waldlaubsaengernest FabianF Samurai-Goroh108 Hyper-E J re342 Gelbbrauenlaubsaenger";
	//ServerConfig::m_owner_less = true;
	//ServerConfig::m_min_start_game_players = 1;
	//ServerConfig::m_server_configurable = true;
	//ServerConfig::m_start_game_counter = 180;
	//ServerConfig::m_player_queue_limit = 2;
	//ServerConfig::m_team_choosing = true;
	//ServerConfig::m_rank_1vs1 = true;

    m_client_server_host_id.store(0);
    m_lobby_players.store(0);
    m_help_message = getGameSetup()->readOrLoadFromFile
        ((std::string) ServerConfig::m_help);

    m_available_commands = "help commands music kick to public "
        "teamchat gnu nognu standings gnu2 gnu2addtrack record tell "
        "installaddon uninstalladdon liststkaddon listlocaladdon "
        "listserveraddon playerhasaddon playeraddonscore serverhasaddon "
		"setfield settrack setkart sethost mode vote";

    m_gnu_elimination = false;
    m_gnu_remained = 0;

	m_player_queue_limit = ServerConfig::m_player_queue_limit;

    m_fixed_lap = ServerConfig::m_fixed_lap_count;

    initAvailableModes();

    std::vector<int> all_k =
        kart_properties_manager->getKartsInGroup("standard");
    std::vector<int> all_t =
        track_manager->getTracksInGroup("standard");
    std::vector<int> all_arenas =
        track_manager->getArenasInGroup("standard", false);
    std::vector<int> all_soccers =
        track_manager->getArenasInGroup("standard", true);
    all_t.insert(all_t.end(), all_arenas.begin(), all_arenas.end());
    all_t.insert(all_t.end(), all_soccers.begin(), all_soccers.end());

    for (int kart : all_k)
    {
        const KartProperties* kp = kart_properties_manager->getKartById(kart);
        // Some distro put kart itself, ignore it online for the rest of stk
        // user
        if (kp->getIdent() == "geeko")
            continue;
        if (!kp->isAddon())
            m_official_kts.first.insert(kp->getIdent());
    }
    for (int track : all_t)
    {
        Track* t = track_manager->getTrack(track);
        if (!t->isAddon())
            m_official_kts.second.insert(t->getIdent());
    }
    updateAddons();

    initAvailableTracks();

    m_rs_state.store(RS_NONE);
    m_last_success_poll_time.store(StkTime::getMonoTimeMs() + 30000);
    m_last_unsuccess_poll_time = StkTime::getMonoTimeMs();
    m_server_owner_id.store(-1);
    m_registered_for_once_only = false;
    setHandleDisconnections(true);
    m_state = SET_PUBLIC_ADDRESS;
    m_save_server_config = true;
    if (ServerConfig::m_ranked)
    {
        Log::info("ServerLobby", "This server will submit ranking scores to "
            "the STK addons server. Don't bother hosting one without the "
            "corresponding permissions, as they would be rejected.");
    }
    m_result_ns = getNetworkString();
    m_result_ns->setSynchronous(true);
    m_items_complete_state = new BareNetworkString();
    m_server_id_online.store(0);
    m_difficulty.store(ServerConfig::m_server_difficulty);
    m_game_mode.store(ServerConfig::m_server_mode);
    m_default_vote = new PeerVote();
    m_player_reports_table_exists = false;
    initDatabase();

    if (ServerConfig::m_soccer_tournament) 
	{
        initTournamentPlayers();
        m_tournament_game = 0;
    }
    if (ServerConfig::m_race_tournament)
	{
		initRaceTournamentPlayers();
		m_tournament_game = 0;
	}
    m_allowed_to_start = true;
    loadTracksQueueFromConfig();
    loadCustomScoring();
    loadWhiteList();
#ifdef ENABLE_WEB_SUPPORT
    m_token_generation_tries.store(0);
    loadAllTokens();
#endif
}   // ServerLobby

//-----------------------------------------------------------------------------
/** Destructor.
 */
ServerLobby::~ServerLobby()
{
    if (m_server_id_online.load() != 0)
    {
        // For child process the request manager will keep on running
        unregisterServer(m_process_type == PT_MAIN ? true : false/*now*/);
    }
    delete m_result_ns;
    delete m_items_complete_state;
    if (m_save_server_config)
        ServerConfig::writeServerConfigToDisk();
    delete m_default_vote;
    destroyDatabase();
}   // ~ServerLobby

//-----------------------------------------------------------------------------
void ServerLobby::initDatabase()
{
#ifdef ENABLE_SQLITE3
    m_last_poll_db_time = StkTime::getMonoTimeMs();
    m_db = NULL;
    m_ip_ban_table_exists = false;
    m_ipv6_ban_table_exists = false;
    m_online_id_ban_table_exists = false;
    m_ip_geolocation_table_exists = false;
    m_ipv6_geolocation_table_exists = false;
    if (!ServerConfig::m_sql_management)
        return;
    const std::string& path = ServerConfig::getConfigDirectory() + "/" +
        ServerConfig::m_database_file.c_str();
    int ret = sqlite3_open_v2(path.c_str(), &m_db,
        SQLITE_OPEN_SHAREDCACHE | SQLITE_OPEN_FULLMUTEX |
        SQLITE_OPEN_READWRITE, NULL);
    if (ret != SQLITE_OK)
    {
        Log::error("ServerLobby", "Cannot open database: %s.",
            sqlite3_errmsg(m_db));
        sqlite3_close(m_db);
        m_db = NULL;
        return;
    }
    sqlite3_busy_handler(m_db, [](void* data, int retry)
        {
            int retry_count = ServerConfig::m_database_timeout / 100;
            if (retry < retry_count)
            {
                sqlite3_sleep(100);
                // Return non-zero to let caller retry again
                return 1;
            }
            // Return zero to let caller return SQLITE_BUSY immediately
            return 0;
        }, NULL);
    sqlite3_create_function(m_db, "insideIPv6CIDR", 2, SQLITE_UTF8, NULL,
        &insideIPv6CIDRSQL, NULL, NULL);
    sqlite3_create_function(m_db, "upperIPv6", 1, SQLITE_UTF8, NULL,
        &upperIPv6SQL, NULL, NULL);
    checkTableExists(ServerConfig::m_ip_ban_table, m_ip_ban_table_exists);
    checkTableExists(ServerConfig::m_ipv6_ban_table, m_ipv6_ban_table_exists);
    checkTableExists(ServerConfig::m_online_id_ban_table,
        m_online_id_ban_table_exists);
    checkTableExists(ServerConfig::m_player_reports_table,
        m_player_reports_table_exists);
    checkTableExists(ServerConfig::m_ip_geolocation_table,
        m_ip_geolocation_table_exists);
    checkTableExists(ServerConfig::m_ipv6_geolocation_table,
        m_ipv6_geolocation_table_exists);
#endif
}   // initDatabase

//-----------------------------------------------------------------------------
void ServerLobby::initServerStatsTable()
{
#ifdef ENABLE_SQLITE3
    if (!ServerConfig::m_sql_management || !m_db)
        return;
    std::string table_name = std::string("v") +
        StringUtils::toString(ServerConfig::m_server_db_version) + "_" +
        ServerConfig::m_server_uid + "_stats";

    std::ostringstream oss;
    oss << "CREATE TABLE IF NOT EXISTS " << table_name << " (\n"
        "    host_id INTEGER UNSIGNED NOT NULL PRIMARY KEY, -- Unique host id in STKHost of each connection session for a STKPeer\n"
        "    ip INTEGER UNSIGNED NOT NULL, -- IP decimal of host\n";
    if (ServerConfig::m_ipv6_connection)
        oss << "    ipv6 TEXT NOT NULL DEFAULT '', -- IPv6 (if exists) in string of host\n";
    oss << "    port INTEGER UNSIGNED NOT NULL, -- Port of host\n"
        "    online_id INTEGER UNSIGNED NOT NULL, -- Online if of the host (0 for offline account)\n"
        "    username TEXT NOT NULL, -- First player name in the host (if the host has splitscreen player)\n"
        "    player_num INTEGER UNSIGNED NOT NULL, -- Number of player(s) from the host, more than 1 if it has splitscreen player\n"
        "    country_code TEXT NULL DEFAULT NULL, -- 2-letter country code of the host\n"
        "    version TEXT NOT NULL, -- SuperTuxKart version of the host\n"
        "    os TEXT NOT NULL, -- Operating system of the host\n"
        "    connected_time TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP, -- Time when connected\n"
        "    disconnected_time TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP, -- Time when disconnected (saved when disconnected)\n"
        "    ping INTEGER UNSIGNED NOT NULL DEFAULT 0, -- Ping of the host\n"
        "    packet_loss INTEGER NOT NULL DEFAULT 0, -- Mean packet loss count from ENet (saved when disconnected)\n"
        "    addon_karts_count INTEGER UNSIGNED NOT NULL DEFAULT 0, -- Number of addon karts of the host\n"
        "    addon_tracks_count INTEGER UNSIGNED NOT NULL DEFAULT 0, -- Number of addon tracks of the host\n"
        "    addon_arenas_count INTEGER UNSIGNED NOT NULL DEFAULT 0, -- Number of addon arenas of the host\n"
        "    addon_soccers_count INTEGER UNSIGNED NOT NULL DEFAULT 0 -- Number of addon soccers of the host\n"
        ") WITHOUT ROWID;";
    std::string query = oss.str();
    sqlite3_stmt* stmt = NULL;
    int ret = sqlite3_prepare_v2(m_db, query.c_str(), -1, &stmt, 0);
    if (ret == SQLITE_OK)
    {
        ret = sqlite3_step(stmt);
        ret = sqlite3_finalize(stmt);
        if (ret == SQLITE_OK)
            m_server_stats_table = table_name;
        else
        {
            Log::error("ServerLobby",
                "Error finalize database for query %s: %s",
                query.c_str(), sqlite3_errmsg(m_db));
        }
    }
    else
    {
        Log::error("ServerLobby", "Error preparing database for query %s: %s",
            query.c_str(), sqlite3_errmsg(m_db));
    }
    if (m_server_stats_table.empty())
        return;

    // Extra default table _countries:
    // Server owner need to initialise this table himself, check NETWORKING.md
    std::string country_table_name = std::string("v") + StringUtils::toString(
        ServerConfig::m_server_db_version) + "_countries";
    query = StringUtils::insertValues(
        "CREATE TABLE IF NOT EXISTS %s (\n"
        "    country_code TEXT NOT NULL PRIMARY KEY UNIQUE, -- Unique 2-letter country code\n"
        "    country_flag TEXT NOT NULL, -- Unicode country flag representation of 2-letter country code\n"
        "    country_name TEXT NOT NULL -- Readable name of this country\n"
        ") WITHOUT ROWID;", country_table_name.c_str());
    easySQLQuery(query);

    // Extra default table _results:
    // Server owner need to initialise this table himself, check NETWORKING.md
    m_results_table_name = std::string("v") + StringUtils::toString(
        ServerConfig::m_server_db_version) + "_" +
        ServerConfig::m_server_uid + "_results";
    query = StringUtils::insertValues(
        "CREATE TABLE IF NOT EXISTS %s (\n"
        "    time TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP, -- Timestamp of the result\n"
        "    username TEXT NOT NULL, -- User who set the result\n"
        "    venue TEXT NOT NULL, -- Track for a race\n"
        "    reverse TEXT NOT NULL, -- Direction\n"
        "    mode TEXT NOT NULL, -- Racing mode\n"
        "    laps INTEGER NOT NULL, -- Number of laps\n"
        "    result REAL NOT NULL -- Elapsed time for a race, possibly with autofinish\n"
        ");", m_results_table_name.c_str());
    easySQLQuery(query);

    // Default views:
    // _full_stats
    // Full stats with ip in human readable format and time played of each
    // players in minutes
    std::string full_stats_view_name = std::string("v") +
        StringUtils::toString(ServerConfig::m_server_db_version) + "_" +
        ServerConfig::m_server_uid + "_full_stats";
    oss.str("");
    oss << "CREATE VIEW IF NOT EXISTS " << full_stats_view_name << " AS\n"
        << "    SELECT host_id, ip,\n"
        << "    ((ip >> 24) & 255) ||'.'|| ((ip >> 16) & 255) ||'.'|| ((ip >>  8) & 255) ||'.'|| ((ip ) & 255) AS ip_readable,\n";
    if (ServerConfig::m_ipv6_connection)
        oss << "    ipv6,";
    oss << "    port, online_id, username, player_num,\n"
        << "    " << m_server_stats_table << ".country_code AS country_code, country_flag, country_name, version, os,\n"
        << "    ROUND((STRFTIME(\"%s\", disconnected_time) - STRFTIME(\"%s\", connected_time)) / 60.0, 2) AS time_played,\n"
        << "    connected_time, disconnected_time, ping, packet_loss, \n"
        << "    addon_karts_count, addon_tracks_count, addon_arenas_count,\n"
        << "    addon_soccers_count FROM " << m_server_stats_table << "\n"
        << "    LEFT JOIN " << country_table_name << " ON "
        <<      country_table_name << ".country_code = " << m_server_stats_table << ".country_code\n"
        << "    ORDER BY connected_time DESC;";
    query = oss.str();
    easySQLQuery(query);

    // _current_players
    // Current players in server with ip in human readable format and time
    // played of each players in minutes
    std::string current_players_view_name = std::string("v") +
        StringUtils::toString(ServerConfig::m_server_db_version) + "_" +
        ServerConfig::m_server_uid + "_current_players";
    oss.str("");
    oss.clear();
    oss << "CREATE VIEW IF NOT EXISTS " << current_players_view_name << " AS\n"
        << "    SELECT host_id, ip,\n"
        << "    ((ip >> 24) & 255) ||'.'|| ((ip >> 16) & 255) ||'.'|| ((ip >>  8) & 255) ||'.'|| ((ip ) & 255) AS ip_readable,\n";
    if (ServerConfig::m_ipv6_connection)
        oss << "    ipv6,";
    oss << "    port, online_id, username, player_num,\n"
        << "    " << m_server_stats_table << ".country_code AS country_code, country_flag, country_name, version, os,\n"
        << "    ROUND((STRFTIME(\"%s\", 'now') - STRFTIME(\"%s\", connected_time)) / 60.0, 2) AS time_played,\n"
        << "    connected_time, ping, "
        << "    addon_karts_count, addon_tracks_count, addon_arenas_count,\n"
        << "    addon_soccers_count FROM " << m_server_stats_table << "\n"
        << "    LEFT JOIN " << country_table_name << " ON "
        <<      country_table_name << ".country_code = " << m_server_stats_table << ".country_code\n"
        << "    WHERE connected_time = disconnected_time;";
    query = oss.str();
    easySQLQuery(query);

    // _player_stats
    // All players with online id and username with their time played stats
    // in this server since creation of this database
    // If sqlite supports window functions (since 3.25), it will include last session player info (ip, country, ping...)
    std::string player_stats_view_name = std::string("v") +
        StringUtils::toString(ServerConfig::m_server_db_version) + "_" +
        ServerConfig::m_server_uid + "_player_stats";
    oss.str("");
    oss.clear();
    if (sqlite3_libversion_number() < 3025000)
    {
        oss << "CREATE VIEW IF NOT EXISTS " << player_stats_view_name << " AS\n"
            << "    SELECT online_id, username, COUNT(online_id) AS num_connections,\n"
            << "    MIN(connected_time) AS first_connected_time,\n"
            << "    MAX(connected_time) AS last_connected_time,\n"
            << "    ROUND(SUM((STRFTIME(\"%s\", disconnected_time) - STRFTIME(\"%s\", connected_time)) / 60.0), 2) AS total_time_played,\n"
            << "    ROUND(AVG((STRFTIME(\"%s\", disconnected_time) - STRFTIME(\"%s\", connected_time)) / 60.0), 2) AS average_time_played,\n"
            << "    ROUND(MIN((STRFTIME(\"%s\", disconnected_time) - STRFTIME(\"%s\", connected_time)) / 60.0), 2) AS min_time_played,\n"
            << "    ROUND(MAX((STRFTIME(\"%s\", disconnected_time) - STRFTIME(\"%s\", connected_time)) / 60.0), 2) AS max_time_played\n"
            << "    FROM " << m_server_stats_table << "\n"
            << "    WHERE online_id != 0 GROUP BY online_id ORDER BY num_connections DESC;";
    }
    else
    {
        oss << "CREATE VIEW IF NOT EXISTS " << player_stats_view_name << " AS\n"
            << "    SELECT a.online_id, a.username, a.ip, a.ip_readable,\n";
        if (ServerConfig::m_ipv6_connection)
            oss << "    a.ipv6,";
        oss << "    a.port, a.player_num,\n"
            << "    a.country_code, a.country_flag, a.country_name, a.version, a.os, a.ping, a.packet_loss,\n"
            << "    b.num_connections, b.first_connected_time, b.first_disconnected_time,\n"
            << "    a.connected_time AS last_connected_time, a.disconnected_time AS last_disconnected_time,\n"
            << "    a.time_played AS last_time_played, b.total_time_played, b.average_time_played,\n"
            << "    b.min_time_played, b.max_time_played\n"
            << "    FROM\n"
            << "    (\n"
            << "        SELECT *,\n"
            << "        ROW_NUMBER() OVER\n"
            << "        (\n"
            << "            PARTITION BY online_id\n"
            << "            ORDER BY connected_time DESC\n"
            << "        ) RowNum\n"
            << "        FROM " << full_stats_view_name << " where online_id != 0\n"
            << "    ) as a\n"
            << "    JOIN\n"
            << "    (\n"
            << "        SELECT online_id, COUNT(online_id) AS num_connections,\n"
            << "        MIN(connected_time) AS first_connected_time,\n"
            << "        MIN(disconnected_time) AS first_disconnected_time,\n"
            << "        ROUND(SUM((STRFTIME(\"%s\", disconnected_time) - STRFTIME(\"%s\", connected_time)) / 60.0), 2) AS total_time_played,\n"
            << "        ROUND(AVG((STRFTIME(\"%s\", disconnected_time) - STRFTIME(\"%s\", connected_time)) / 60.0), 2) AS average_time_played,\n"
            << "        ROUND(MIN((STRFTIME(\"%s\", disconnected_time) - STRFTIME(\"%s\", connected_time)) / 60.0), 2) AS min_time_played,\n"
            << "        ROUND(MAX((STRFTIME(\"%s\", disconnected_time) - STRFTIME(\"%s\", connected_time)) / 60.0), 2) AS max_time_played\n"
            << "        FROM " << m_server_stats_table << " WHERE online_id != 0 GROUP BY online_id\n"
            << "    ) AS b\n"
            << "    ON b.online_id = a.online_id\n"
            << "    WHERE RowNum = 1 ORDER BY num_connections DESC;\n";
    }
    query = oss.str();
    easySQLQuery(query);

    uint32_t last_host_id = 0;
    query = StringUtils::insertValues("SELECT MAX(host_id) FROM %s;",
        m_server_stats_table.c_str());
    ret = sqlite3_prepare_v2(m_db, query.c_str(), -1, &stmt, 0);
    if (ret == SQLITE_OK)
    {
        ret = sqlite3_step(stmt);
        if (ret == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL)
        {
            last_host_id = (unsigned)sqlite3_column_int64(stmt, 0);
            Log::info("ServerLobby", "%u was last server session max host id.",
                last_host_id);
        }
        ret = sqlite3_finalize(stmt);
        if (ret != SQLITE_OK)
        {
            Log::error("ServerLobby",
                "Error finalize database for query %s: %s",
                query.c_str(), sqlite3_errmsg(m_db));
            m_server_stats_table = "";
        }
    }
    else
    {
        Log::error("ServerLobby", "Error preparing database for query %s: %s",
            query.c_str(), sqlite3_errmsg(m_db));
        m_server_stats_table = "";
    }
    STKHost::get()->setNextHostId(last_host_id);

    // Update disconnected time (if stk crashed it will not be written)
    query = StringUtils::insertValues(
        "UPDATE %s SET disconnected_time = datetime('now') "
        "WHERE connected_time = disconnected_time;",
        m_server_stats_table.c_str());
    easySQLQuery(query);
#endif
}   // initServerStatsTable

//-----------------------------------------------------------------------------
void ServerLobby::destroyDatabase()
{
#ifdef ENABLE_SQLITE3
    auto peers = STKHost::get()->getPeers();
    for (auto& peer : peers)
        writeDisconnectInfoTable(peer.get());
    if (m_db != NULL)
        sqlite3_close(m_db);
#endif
}   // destroyDatabase

//-----------------------------------------------------------------------------
void ServerLobby::writeDisconnectInfoTable(STKPeer* peer)
{
#ifdef ENABLE_SQLITE3
    if (m_server_stats_table.empty())
        return;
    std::string query = StringUtils::insertValues(
        "UPDATE %s SET disconnected_time = datetime('now'), "
        "ping = %d, packet_loss = %d "
        "WHERE host_id = %u;", m_server_stats_table.c_str(),
        peer->getAveragePing(), peer->getPacketLoss(),
        peer->getHostId());
    easySQLQuery(query);
#endif
}   // writeDisconnectInfoTable

//-----------------------------------------------------------------------------
void ServerLobby::updateAddons()
{
    m_addon_kts.first.clear();
    m_addon_kts.second.clear();
    m_addon_arenas.clear();
    m_addon_soccers.clear();

    std::set<std::string> total_addons;
    for (unsigned i = 0; i < kart_properties_manager->getNumberOfKarts(); i++)
    {
        const KartProperties* kp =
            kart_properties_manager->getKartById(i);
        if (kp->isAddon())
            total_addons.insert(kp->getIdent());
    }
    for (unsigned i = 0; i < track_manager->getNumberOfTracks(); i++)
    {
        const Track* track = track_manager->getTrack(i);
        if (track->isAddon())
            total_addons.insert(track->getIdent());
    }

    for (auto& addon : total_addons)
    {
        const KartProperties* kp = kart_properties_manager->getKart(addon);
        if (kp && kp->isAddon())
        {
            m_addon_kts.first.insert(kp->getIdent());
            continue;
        }
        Track* t = track_manager->getTrack(addon);
        if (!t || !t->isAddon() || t->isInternal())
            continue;
        if (t->isArena())
            m_addon_arenas.insert(t->getIdent());
        else if (t->isSoccer())
            m_addon_soccers.insert(t->getIdent());
        else
            m_addon_kts.second.insert(t->getIdent());
    }

    auto all_k = kart_properties_manager->getAllAvailableKarts();
    if (all_k.size() >= 65536)
        all_k.resize(65535);
    //if (ServerConfig::m_live_players)   TODO Here, the addon karts are removed.
    //    m_available_kts.first = m_official_kts.first;
    //else
        m_available_kts.first = { all_k.begin(), all_k.end() };
    m_entering_kts = m_available_kts;
}   // updateAddons

//-----------------------------------------------------------------------------
/** Called whenever server is reset or game mode is changed.
 */
void ServerLobby::updateTracksForMode()
{
    auto all_t = track_manager->getAllTrackIdentifiers();
    if (all_t.size() >= 65536)
        all_t.resize(65535);
    m_available_kts.second = { all_t.begin(), all_t.end() };
    RaceManager::MinorRaceModeType m =
        ServerConfig::getLocalGameMode(m_game_mode.load()).first;
    switch (m)
    {
        case RaceManager::MINOR_MODE_NORMAL_RACE:
        case RaceManager::MINOR_MODE_TIME_TRIAL:
        case RaceManager::MINOR_MODE_FOLLOW_LEADER:
        {
            auto it = m_available_kts.second.begin();
            while (it != m_available_kts.second.end())
            {
                Track* t =  track_manager->getTrack(*it);
                if (t->isArena() || t->isSoccer() || t->isInternal())
                {
                    it = m_available_kts.second.erase(it);
                }
                else
                    it++;
            }
            break;
        }
        case RaceManager::MINOR_MODE_FREE_FOR_ALL:
        case RaceManager::MINOR_MODE_CAPTURE_THE_FLAG:
        {
            auto it = m_available_kts.second.begin();
            while (it != m_available_kts.second.end())
            {
                Track* t =  track_manager->getTrack(*it);
                if (RaceManager::get()->getMinorMode() ==
                    RaceManager::MINOR_MODE_CAPTURE_THE_FLAG)
                {
                    if (!t->isCTF() || t->isInternal())
                    {
                        it = m_available_kts.second.erase(it);
                    }
                    else
                        it++;
                }
                else
                {
                    if (!t->isArena() ||  t->isInternal())
                    {
                        it = m_available_kts.second.erase(it);
                    }
                    else
                        it++;
                }
            }
            break;
        }
        case RaceManager::MINOR_MODE_SOCCER:
        {
            auto it = m_available_kts.second.begin();
            while (it != m_available_kts.second.end())
            {
                Track* t =  track_manager->getTrack(*it);
                if (!t->isSoccer() || t->isInternal())
                {
                    it = m_available_kts.second.erase(it);
                }
                else
                    it++;
            }
            break;
        }
        default:
            assert(false);
            break;
    }
    /*if (m_restricting_config)
    {
        std::set<std::string> erase_tracks;
        auto it = m_available_kts.second.begin();
        while (it != m_available_kts.second.end())
        {
            if (m_inverted_config_restriction ^
                (m_config_available_tracks.find(*it) ==
                    m_config_available_tracks.end()))
                erase_tracks.insert(*it);
            it++;
        }
        for (const std::string& track : erase_tracks) {
            m_available_kts.second.erase(track);
        }
    }*/
    m_entering_kts = m_available_kts;
}   // updateTracksForMode

//-----------------------------------------------------------------------------
void ServerLobby::setup()
{
    LobbyProtocol::setup();
    m_battle_hit_capture_limit = 0;
    m_battle_time_limit = 0.0f;
    m_item_seed = 0;
    m_winner_peer_id = 0;
    m_client_starting_time = 0;
    m_ai_count = 0;
    auto players = STKHost::get()->getPlayersForNewGame();
    if (m_game_setup->isGrandPrix() && !m_game_setup->isGrandPrixStarted())
    {
        for (auto player : players)
            player->resetGrandPrixData();
    }
    if (!m_game_setup->isGrandPrix() || !m_game_setup->isGrandPrixStarted())
    {
        for (auto player : players)
            player->setKartName("");
    }
    if (auto ai = m_ai_peer.lock())
    {
        for (auto player : ai->getPlayerProfiles())
            player->setKartName("");
    }
    for (auto ai : m_ai_profiles)
        ai->setKartName("");

    StateManager::get()->resetActivePlayers();
    // We use maximum 16bit unsigned limit
    auto all_k = kart_properties_manager->getAllAvailableKarts();
    if (all_k.size() >= 65536)
        all_k.resize(65535);
    if (ServerConfig::m_live_players)
        m_available_kts.first = m_official_kts.first;
    else
        m_available_kts.first = { all_k.begin(), all_k.end() };
    NetworkConfig::get()->setTuxHitboxAddon(ServerConfig::m_live_players);
    updateTracksForMode();

    m_server_has_loaded_world.store(false);

    // Initialise the data structures to detect if all clients and 
    // the server are ready:
    resetPeersReady();
    m_timeout.store(std::numeric_limits<int64_t>::max());
    m_server_started_at = m_server_delay = 0;

    Log::info("ServerLobby", "Resetting the server to its initial state.");
}   // setup

//-----------------------------------------------------------------------------
bool ServerLobby::notifyEvent(Event* event)
{
    assert(m_game_setup); // assert that the setup exists
    if (event->getType() != EVENT_TYPE_MESSAGE)
        return true;

    NetworkString &data = event->data();
    assert(data.size()); // message not empty
    uint8_t message_type;
    message_type = data.getUInt8();
    Log::info("ServerLobby", "Synchronous message of type %d received.",
              message_type);
    switch (message_type)
    {
    case LE_RACE_FINISHED_ACK: playerFinishedResult(event);   break;
    case LE_LIVE_JOIN:         liveJoinRequest(event);        break;
    case LE_CLIENT_LOADED_WORLD: finishedLoadingLiveJoinClient(event); break;
    case LE_KART_INFO: handleKartInfo(event); break;
    case LE_CLIENT_BACK_LOBBY: clientInGameWantsToBackLobby(event); break;
    default: Log::error("ServerLobby", "Unknown message of type %d - ignored.",
                        message_type);
             break;
    }   // switch message_type
    return true;
}   // notifyEvent

//-----------------------------------------------------------------------------
void ServerLobby::handleChat(Event* event)
{
    if (!checkDataSize(event, 1) || !ServerConfig::m_chat) return;

    // Update so that the peer is not kicked
    event->getPeer()->updateLastActivity();
    const bool sender_in_game = event->getPeer()->isWaitingForGame();

    int64_t last_message = event->getPeer()->getLastMessage();
    int64_t elapsed_time = (int64_t)StkTime::getMonoTimeMs() - last_message;

    // Read ServerConfig for formula and details
    if (ServerConfig::m_chat_consecutive_interval > 0 &&
        elapsed_time < ServerConfig::m_chat_consecutive_interval * 1000)
        event->getPeer()->updateConsecutiveMessages(true);
    else
        event->getPeer()->updateConsecutiveMessages(false);

    if (ServerConfig::m_chat_consecutive_interval > 0 &&
        event->getPeer()->getConsecutiveMessages() >
        ServerConfig::m_chat_consecutive_interval / 2)
    {
        NetworkString* chat = getNetworkString();
        chat->setSynchronous(true);
        core::stringw warn = "Spam detected";
        chat->addUInt8(LE_CHAT).encodeString16(warn);
        event->getPeer()->sendPacket(chat, true/*reliable*/);
        delete chat;
        return;
    }

    core::stringw message;
    event->data().decodeString16(&message, 360/*max_len*/);

    KartTeam target_team = KART_TEAM_NONE;
    if (event->data().size() > 0)
        target_team = (KartTeam)event->data().getUInt8();

    if (message.size() > 0)
    {
        // Red or blue square emoji
        if (target_team == KART_TEAM_RED)
            message = StringUtils::utf32ToWide({0x1f7e5, 0x20}) + message;
        else if (target_team == KART_TEAM_BLUE)
            message = StringUtils::utf32ToWide({0x1f7e6, 0x20}) + message;

        NetworkString* chat = getNetworkString();
        chat->setSynchronous(true);
        chat->addUInt8(LE_CHAT).encodeString16(message);
        const bool game_started = m_state.load() != WAITING_FOR_START_GAME;
        STKPeer* sender = event->getPeer();
        auto can_receive = m_message_receivers[sender];
        bool team_speak = m_team_speakers.find(sender) != m_team_speakers.end();
        team_speak &= (
            RaceManager::get()->getMinorMode() == RaceManager::MINOR_MODE_SOCCER ||
            RaceManager::get()->getMinorMode() == RaceManager::MINOR_MODE_CAPTURE_THE_FLAG
        );
        std::set<KartTeam> teams;
        for (auto& profile: sender->getPlayerProfiles())
            teams.insert(profile->getTeam());
        bool tournament_limit = false;
        std::set<std::string> important_players;
        if (ServerConfig::m_soccer_tournament && m_tournament_limited_chat)
        {
            tournament_limit = true;
            for (auto& profile: sender->getPlayerProfiles())
            {
                std::string name = StringUtils::wideToUtf8(
                    profile->getName());
                if (m_tournament_referees.count(name) > 0 ||
                    m_tournament_red_players.count(name) > 0 ||
                    m_tournament_blue_players.count(name) > 0)
                    tournament_limit = false;
            }
        }
        if (tournament_limit)
        {
            for (const std::string& s: m_tournament_referees)
                important_players.insert(s);
            for (const std::string& s: m_tournament_red_players)
                important_players.insert(s);
            for (const std::string& s: m_tournament_blue_players)
                important_players.insert(s);
        }

        STKHost::get()->sendPacketToAllPeersWith(
            [game_started, sender_in_game, target_team, can_receive,
                sender, team_speak, teams, tournament_limit,
                important_players](STKPeer* p)
            {
                if (sender == p)
                    return true;
                if (game_started)
                {
                    if (p->isWaitingForGame() && !sender_in_game)
                        return false;
                    if (!p->isWaitingForGame() && sender_in_game)
                        return false;
                    if (tournament_limit) {
                        bool all_are_important = true;
                        for (auto& player : p->getPlayerProfiles())
                        {
                            std::string name = StringUtils::wideToUtf8(
                                player->getName());
                            if (important_players.count(name) == 0)
                            {
                                all_are_important = false;
                                break;
                            }
                        }
                        if (all_are_important)
                            return false;
                    }
                    if (target_team != KART_TEAM_NONE)
                    {
                        if (p->isSpectator())
                            return false;
                        for (auto& player : p->getPlayerProfiles())
                        {
                            if (player->getTeam() == target_team)
                                return true;
                        }
                        return false;
                    }
                }
                if (team_speak)
                {
                    for (auto& profile: p->getPlayerProfiles())
                        if (teams.count(profile->getTeam()) > 0)
                            return true;
                    return false;
                }
                if (can_receive.empty())
                    return true;
                for (auto& profile : p->getPlayerProfiles())
                {
                    if (can_receive.find(profile->getName()) !=
                        can_receive.end())
                    {
                        return true;
                    }
                }
                return false;
            }, chat);
        event->getPeer()->updateLastMessage();
        delete chat;
    }
}   // handleChat

//-----------------------------------------------------------------------------
void ServerLobby::changeTeam(Event* event)
{
    if (!ServerConfig::m_team_choosing ||
        !RaceManager::get()->teamEnabled())
        return;
    if (!checkDataSize(event, 1)) return;
    NetworkString& data = event->data();
    uint8_t local_id = data.getUInt8();
    auto& player = event->getPeer()->getPlayerProfiles().at(local_id);
    auto red_blue = STKHost::get()->getAllPlayersTeamInfo();
    if (ServerConfig::m_soccer_tournament) {
        return; // message here?
    }
    // At most 7 players on each team (for live join)
    if (player->getTeam() == KART_TEAM_BLUE)
    {
        if (red_blue.first >= 7 && !ServerConfig::m_free_teams)
            return;
        player->setTeam(KART_TEAM_RED);
    }
    else
    {
        if (red_blue.second >= 7 && !ServerConfig::m_free_teams)
            return;
        player->setTeam(KART_TEAM_BLUE);
    }
    updatePlayerList();
}   // changeTeam

//-----------------------------------------------------------------------------
void ServerLobby::kickHost(Event* event)
{
    if (m_server_owner.lock() != event->getPeerSP())
        return;
    if (!ServerConfig::m_kicks_allowed)
    {
        std::string msg = "Kicking players is not allowed on this server";
        auto crown = event->getPeerSP();
        sendStringToPeer(msg, crown);
        return;
    }
    if (!checkDataSize(event, 4)) return;
    NetworkString& data = event->data();
    uint32_t host_id = data.getUInt32();
    std::shared_ptr<STKPeer> peer = STKHost::get()->findPeerByHostId(host_id);
    // Ignore kicking ai peer if ai handling is on
    if (peer && (!ServerConfig::m_ai_handling || !peer->isAIPeer()))
    {
		std::string peer_username = StringUtils::wideToUtf8(
			peer->getPlayerProfiles()[0]->getName());
		/*
        if (peer->isAngryHost())
        {
            std::string msg = "This player is the owner of this server, "
                "and is protected from your actions now";
            auto crown = event->getPeerSP();
            sendStringToPeer(msg, crown);
            return;
        }
		*/
        if (!peer->hasPlayerProfiles())
        {
            Log::info("ServerLobby", "Crown player %s kicks a player", peer_username.c_str());
        }
        else
        {
            auto npp = peer->getPlayerProfiles()[0];
            std::string player_name = StringUtils::wideToUtf8(npp->getName());
			Log::info("ServerLobby", "Crown player %s kicks %s", peer_username.c_str(), player_name.c_str());
        }
        peer->kick();
        if (ServerConfig::m_track_kicks) {
            std::string auto_report = "[ Auto report caused by kick ]";
            writeOwnReport(peer.get(), event->getPeerSP().get(), auto_report);
        }
    }
}   // kickHost

//-----------------------------------------------------------------------------
bool ServerLobby::notifyEventAsynchronous(Event* event)
{
    assert(m_game_setup); // assert that the setup exists
    if (event->getType() == EVENT_TYPE_MESSAGE)
    {
        NetworkString &data = event->data();
        assert(data.size()); // message not empty
        uint8_t message_type;
        message_type = data.getUInt8();
        Log::info("ServerLobby", "Message of type %d received.",
                  message_type);
        switch(message_type)
        {
        case LE_CONNECTION_REQUESTED: connectionRequested(event); break;
        case LE_KART_SELECTION: kartSelectionRequested(event);    break;
        case LE_CLIENT_LOADED_WORLD: finishedLoadingWorldClient(event); break;
        case LE_VOTE: handlePlayerVote(event);                    break;
        case LE_KICK_HOST: kickHost(event);                       break;
        case LE_CHANGE_TEAM: changeTeam(event);                   break;
        case LE_REQUEST_BEGIN: startSelection(event);             break;
        case LE_CHAT: handleChat(event);                          break;
        case LE_CONFIG_SERVER: handleServerConfiguration(event);  break;
        case LE_CHANGE_HANDICAP: changeHandicap(event);           break;
        case LE_CLIENT_BACK_LOBBY:
            clientSelectingAssetsWantsToBackLobby(event);         break;
        case LE_REPORT_PLAYER: writePlayerReport(event);          break;
        case LE_ASSETS_UPDATE:
            handleAssets(event->data(), event->getPeer());        break;
        case LE_COMMAND:
            handleServerCommand(event, event->getPeerSP());       break;
        default:                                                  break;
        }   // switch
    } // if (event->getType() == EVENT_TYPE_MESSAGE)
    else if (event->getType() == EVENT_TYPE_DISCONNECTED)
    {
        clientDisconnected(event);
    } // if (event->getType() == EVENT_TYPE_DISCONNECTED)
    return true;
}   // notifyEventAsynchronous

//-----------------------------------------------------------------------------
#ifdef ENABLE_SQLITE3
/* Every 1 minute STK will poll database:
 * 1. Set disconnected time to now for non-exists host.
 * 2. Clear expired player reports if necessary
 * 3. Kick active peer from ban list
 */
void ServerLobby::pollDatabase()
{
    if (!ServerConfig::m_sql_management || !m_db)
        return;

    if (StkTime::getMonoTimeMs() < m_last_poll_db_time + 60000)
        return;

    m_last_poll_db_time = StkTime::getMonoTimeMs();

    if (m_ip_ban_table_exists)
    {
        std::string query =
            "SELECT ip_start, ip_end, reason, description FROM ";
        query += ServerConfig::m_ip_ban_table;
        query += " WHERE datetime('now') > datetime(starting_time) AND "
            "(expired_days is NULL OR datetime"
            "(starting_time, '+'||expired_days||' days') > datetime('now'));";
        auto peers = STKHost::get()->getPeers();
        sqlite3_exec(m_db, query.c_str(),
            [](void* ptr, int count, char** data, char** columns)
            {
                std::vector<std::shared_ptr<STKPeer> >* peers_ptr =
                    (std::vector<std::shared_ptr<STKPeer> >*)ptr;
                for (std::shared_ptr<STKPeer>& p : *peers_ptr)
                {
                    // IPv4 ban list atm
                    if (p->isAIPeer() || p->getAddress().isIPv6())
                        continue;

                    uint32_t ip_start = 0;
                    uint32_t ip_end = 0;
                    if (!StringUtils::fromString(data[0], ip_start))
                        continue;
                    if (!StringUtils::fromString(data[1], ip_end))
                        continue;
                    uint32_t peer_addr = p->getAddress().getIP();
                    if (ip_start <= peer_addr && ip_end >= peer_addr)
                    {
                        Log::info("ServerLobby",
                            "Kick %s, reason: %s, description: %s",
                            p->getAddress().toString().c_str(),
                            data[2], data[3]);
                        p->kick();
                    }
                }
                return 0;
            }, &peers, NULL);
    }

    if (m_ipv6_ban_table_exists)
    {
        std::string query =
            "SELECT ipv6_cidr, reason, description FROM ";
        query += ServerConfig::m_ipv6_ban_table;
        query += " WHERE datetime('now') > datetime(starting_time) AND "
            "(expired_days is NULL OR datetime"
            "(starting_time, '+'||expired_days||' days') > datetime('now'));";
        auto peers = STKHost::get()->getPeers();
        sqlite3_exec(m_db, query.c_str(),
            [](void* ptr, int count, char** data, char** columns)
            {
                std::vector<std::shared_ptr<STKPeer> >* peers_ptr =
                    (std::vector<std::shared_ptr<STKPeer> >*)ptr;
                for (std::shared_ptr<STKPeer>& p : *peers_ptr)
                {
                    std::string ipv6;
                    if (p->getAddress().isIPv6())
                        ipv6 = p->getAddress().toString(false);
                    // IPv6 ban list atm
                    if (p->isAIPeer() || ipv6.empty())
                        continue;

                    char* ipv6_cidr = data[0];
                    if (insideIPv6CIDR(ipv6_cidr, ipv6.c_str()) == 1)
                    {
                        Log::info("ServerLobby",
                            "Kick %s, reason: %s, description: %s",
                            ipv6.c_str(), data[1], data[2]);
                        p->kick();
                    }
                }
                return 0;
            }, &peers, NULL);
    }

    if (m_online_id_ban_table_exists)
    {
        std::string query = "SELECT online_id, reason, description FROM ";
        query += ServerConfig::m_online_id_ban_table;
        query += " WHERE datetime('now') > datetime(starting_time) AND "
            "(expired_days is NULL OR datetime"
            "(starting_time, '+'||expired_days||' days') > datetime('now'));";
        auto peers = STKHost::get()->getPeers();
        sqlite3_exec(m_db, query.c_str(),
            [](void* ptr, int count, char** data, char** columns)
            {
                std::vector<std::shared_ptr<STKPeer> >* peers_ptr =
                    (std::vector<std::shared_ptr<STKPeer> >*)ptr;
                for (std::shared_ptr<STKPeer>& p : *peers_ptr)
                {
                    if (p->isAIPeer()
                        || p->getPlayerProfiles().empty())
                        continue;

                    uint32_t online_id = 0;
                    if (!StringUtils::fromString(data[0], online_id))
                        continue;
                    if (online_id == p->getPlayerProfiles()[0]->getOnlineId())
                    {
                        Log::info("ServerLobby",
                            "Kick %s, reason: %s, description: %s",
                            p->getAddress().toString().c_str(),
                            data[1], data[2]);
                        p->kick();
                    }
                }
                return 0;
            }, &peers, NULL);
    }

    if (m_player_reports_table_exists &&
        ServerConfig::m_player_reports_expired_days != 0.0f)
    {
        std::string query = StringUtils::insertValues(
            "DELETE FROM %s "
            "WHERE datetime"
            "(reported_time, '+%f days') < datetime('now');",
            ServerConfig::m_player_reports_table.c_str(),
            ServerConfig::m_player_reports_expired_days);
        easySQLQuery(query);
    }
    if (m_server_stats_table.empty())
        return;

    std::string query;
    auto peers = STKHost::get()->getPeers();
    std::vector<uint32_t> exist_hosts;
    if (!peers.empty())
    {
        for (auto& peer : peers)
        {
            if (!peer->isValidated())
                continue;
            exist_hosts.push_back(peer->getHostId());
        }
    }
    if (peers.empty() || exist_hosts.empty())
    {
        query = StringUtils::insertValues(
            "UPDATE %s SET disconnected_time = datetime('now') "
            "WHERE connected_time = disconnected_time;",
            m_server_stats_table.c_str());
    }
    else
    {
        std::ostringstream oss;
        oss << "UPDATE " << m_server_stats_table
            << "    SET disconnected_time = datetime('now')"
            << "    WHERE connected_time = disconnected_time AND"
            << "    host_id NOT IN (";
        for (unsigned i = 0; i < exist_hosts.size(); i++)
        {
            oss << exist_hosts[i];
            if (i != (exist_hosts.size() - 1))
                oss << ",";
        }
        oss << ");";
        query = oss.str();
    }
    easySQLQuery(query);
}   // pollDatabase

//-----------------------------------------------------------------------------
/** Run simple query with write lock waiting and optional function, this
 *  function has no callback for the return (if any) by the query.
 *  Return true if no error occurs
 */
bool ServerLobby::easySQLQuery(const std::string& query,
                   std::function<void(sqlite3_stmt* stmt)> bind_function) const
{
    if (!m_db)
        return false;
    sqlite3_stmt* stmt = NULL;
    int ret = sqlite3_prepare_v2(m_db, query.c_str(), -1, &stmt, 0);
    if (ret == SQLITE_OK)
    {
        if (bind_function)
            bind_function(stmt);
        ret = sqlite3_step(stmt);
        ret = sqlite3_finalize(stmt);
        if (ret != SQLITE_OK)
        {
            Log::error("ServerLobby",
                "Error finalize database for easy query %s: %s",
                query.c_str(), sqlite3_errmsg(m_db));
            return false;
        }
    }
    else
    {
        Log::error("ServerLobby",
            "Error preparing database for easy query %s: %s",
            query.c_str(), sqlite3_errmsg(m_db));
        return false;
    }
    return true;
}   // easySQLQuery

//-----------------------------------------------------------------------------
std::pair<bool, std::vector<std::vector<std::string>>>
ServerLobby::vectorSQLQuery(const std::string& query,
    int columns, std::function<void(sqlite3_stmt* stmt)> bind_function) const
{
    std::vector<std::vector<std::string>> ans(columns);
    if (!m_db)
        return {false, {}};
    sqlite3_stmt* stmt = NULL;
    int ret = sqlite3_prepare_v2(m_db, query.c_str(), -1, &stmt, 0);
    if (ret == SQLITE_OK)
    {
        if (bind_function)
            bind_function(stmt);
        ret = sqlite3_step(stmt);
        while (sqlite3_column_text(stmt, 0))
        {
            for (int i = 0; i < columns; i++)
            {
                ans[i].push_back(std::string(
                    (char*)sqlite3_column_text(stmt, i)));
            }
            ret = sqlite3_step(stmt);
        }
        ret = sqlite3_finalize(stmt);
        if (ret != SQLITE_OK)
        {
            Log::error("ServerLobby",
                "Error finalize database for vector query %s: %s",
                query.c_str(), sqlite3_errmsg(m_db));
            return {false, {}};
        }
    }
    else
    {
        Log::error("ServerLobby",
            "Error preparing database for vector query %s: %s",
            query.c_str(), sqlite3_errmsg(m_db));
        return {false, {}};
    }
    return {true, ans};
}   // vectorSQLQuery

//-----------------------------------------------------------------------------
/* Write true to result if table name exists in database. */
void ServerLobby::checkTableExists(const std::string& table, bool& result)
{
    if (!m_db)
        return;
    sqlite3_stmt* stmt = NULL;
    if (!table.empty())
    {
        std::string query = StringUtils::insertValues(
            "SELECT count(type) FROM sqlite_master "
            "WHERE type='table' AND name='%s';", table.c_str());

        int ret = sqlite3_prepare_v2(m_db, query.c_str(), -1, &stmt, 0);
        if (ret == SQLITE_OK)
        {
            ret = sqlite3_step(stmt);
            if (ret == SQLITE_ROW)
            {
                int number = sqlite3_column_int(stmt, 0);
                if (number == 1)
                {
                    Log::info("ServerLobby", "Table named %s will used.",
                        table.c_str());
                    result = true;
                }
            }
            ret = sqlite3_finalize(stmt);
            if (ret != SQLITE_OK)
            {
                Log::error("ServerLobby",
                    "Error finalize database for query %s: %s",
                    query.c_str(), sqlite3_errmsg(m_db));
            }
        }
    }
    if (!result && !table.empty())
    {
        Log::warn("ServerLobby", "Table named %s not found in database.",
            table.c_str());
    }
}   // checkTableExists

//-----------------------------------------------------------------------------
std::string ServerLobby::ip2Country(const SocketAddress& addr) const
{
    if (!m_db || !m_ip_geolocation_table_exists || addr.isLAN())
        return "";

    std::string cc_code;
    std::string query = StringUtils::insertValues(
        "SELECT country_code FROM %s "
        "WHERE `ip_start` <= %d AND `ip_end` >= %d "
        "ORDER BY `ip_start` DESC LIMIT 1;",
        ServerConfig::m_ip_geolocation_table.c_str(), addr.getIP(),
        addr.getIP());

    sqlite3_stmt* stmt = NULL;
    int ret = sqlite3_prepare_v2(m_db, query.c_str(), -1, &stmt, 0);
    if (ret == SQLITE_OK)
    {
        ret = sqlite3_step(stmt);
        if (ret == SQLITE_ROW)
        {
            const char* country_code = (char*)sqlite3_column_text(stmt, 0);
            cc_code = country_code;
        }
        ret = sqlite3_finalize(stmt);
        if (ret != SQLITE_OK)
        {
            Log::error("ServerLobby",
                "Error finalize database for query %s: %s",
                query.c_str(), sqlite3_errmsg(m_db));
        }
    }
    else
    {
        Log::error("ServerLobby", "Error preparing database for query %s: %s",
            query.c_str(), sqlite3_errmsg(m_db));
        return "";
    }
    return cc_code;
}   // ip2Country

//-----------------------------------------------------------------------------
std::string ServerLobby::ipv62Country(const SocketAddress& addr) const
{
    if (!m_db || !m_ipv6_geolocation_table_exists)
        return "";

    std::string cc_code;
    const std::string& ipv6 = addr.toString(false/*show_port*/);
    std::string query = StringUtils::insertValues(
        "SELECT country_code FROM %s "
        "WHERE `ip_start` <= upperIPv6(\"%s\") AND `ip_end` >= upperIPv6(\"%s\") "
        "ORDER BY `ip_start` DESC LIMIT 1;",
        ServerConfig::m_ipv6_geolocation_table.c_str(), ipv6.c_str(),
        ipv6.c_str());

    sqlite3_stmt* stmt = NULL;
    int ret = sqlite3_prepare_v2(m_db, query.c_str(), -1, &stmt, 0);
    if (ret == SQLITE_OK)
    {
        ret = sqlite3_step(stmt);
        if (ret == SQLITE_ROW)
        {
            const char* country_code = (char*)sqlite3_column_text(stmt, 0);
            cc_code = country_code;
        }
        ret = sqlite3_finalize(stmt);
        if (ret != SQLITE_OK)
        {
            Log::error("ServerLobby",
                "Error finalize database for query %s: %s",
                query.c_str(), sqlite3_errmsg(m_db));
        }
    }
    else
    {
        Log::error("ServerLobby", "Error preparing database for query %s: %s",
            query.c_str(), sqlite3_errmsg(m_db));
        return "";
    }
    return cc_code;
}   // ipv62Country

#endif

//-----------------------------------------------------------------------------
void ServerLobby::writePlayerReport(Event* event)
{
#ifdef ENABLE_SQLITE3
    if (!m_db || !m_player_reports_table_exists)
        return;
    STKPeer* reporter = event->getPeer();
    if (!reporter->hasPlayerProfiles())
        return;
    auto reporter_npp = reporter->getPlayerProfiles()[0];

    uint32_t reporting_host_id = event->data().getUInt32();
    core::stringw info;
    event->data().decodeString16(&info);
    if (info.empty())
        return;

    auto reporting_peer = STKHost::get()->findPeerByHostId(reporting_host_id);
    if (!reporting_peer || !reporting_peer->hasPlayerProfiles())
        return;
    auto reporting_npp = reporting_peer->getPlayerProfiles()[0];

    std::string query;
    if (ServerConfig::m_ipv6_connection)
    {
        query = StringUtils::insertValues(
            "INSERT INTO %s "
            "(server_uid, reporter_ip, reporter_ipv6, reporter_online_id, reporter_username, "
            "info, reporting_ip, reporting_ipv6, reporting_online_id, reporting_username) "
            "VALUES (?, %u, \"%s\", %u, ?, ?, %u, \"%s\", %u, ?);",
            ServerConfig::m_player_reports_table.c_str(),
            !reporter->getAddress().isIPv6() ? reporter->getAddress().getIP() : 0,
            reporter->getAddress().isIPv6() ? reporter->getAddress().toString(false) : "",
            reporter_npp->getOnlineId(),
            !reporting_peer->getAddress().isIPv6() ? reporting_peer->getAddress().getIP() : 0,
            reporting_peer->getAddress().isIPv6() ? reporting_peer->getAddress().toString(false) : "",
            reporting_npp->getOnlineId());
    }
    else
    {
        query = StringUtils::insertValues(
            "INSERT INTO %s "
            "(server_uid, reporter_ip, reporter_online_id, reporter_username, "
            "info, reporting_ip, reporting_online_id, reporting_username) "
            "VALUES (?, %u, %u, ?, ?, %u, %u, ?);",
            ServerConfig::m_player_reports_table.c_str(),
            reporter->getAddress().getIP(), reporter_npp->getOnlineId(),
            reporting_peer->getAddress().getIP(), reporting_npp->getOnlineId());
    }
    bool written = easySQLQuery(query,
        [reporter_npp, reporting_npp, info](sqlite3_stmt* stmt)
        {
            // SQLITE_TRANSIENT to copy string
            if (sqlite3_bind_text(stmt, 1, ServerConfig::m_server_uid.c_str(),
                -1, SQLITE_TRANSIENT) != SQLITE_OK)
            {
                Log::error("easySQLQuery", "Failed to bind %s.",
                    ServerConfig::m_server_uid.c_str());
            }
            if (sqlite3_bind_text(stmt, 2,
                StringUtils::wideToUtf8(reporter_npp->getName()).c_str(),
                -1, SQLITE_TRANSIENT) != SQLITE_OK)
            {
                Log::error("easySQLQuery", "Failed to bind %s.",
                    StringUtils::wideToUtf8(reporter_npp->getName()).c_str());
            }
            if (sqlite3_bind_text(stmt, 3,
                StringUtils::wideToUtf8(info).c_str(),
                -1, SQLITE_TRANSIENT) != SQLITE_OK)
            {
                Log::error("easySQLQuery", "Failed to bind %s.",
                    StringUtils::wideToUtf8(info).c_str());
            }
            if (sqlite3_bind_text(stmt, 4,
                StringUtils::wideToUtf8(reporting_npp->getName()).c_str(),
                -1, SQLITE_TRANSIENT) != SQLITE_OK)
            {
                Log::error("easySQLQuery", "Failed to bind %s.",
                    StringUtils::wideToUtf8(reporting_npp->getName()).c_str());
            }
        });
    if (written)
    {
        NetworkString* success = getNetworkString();
        success->setSynchronous(true);
        success->addUInt8(LE_REPORT_PLAYER).addUInt8(1)
            .encodeString(reporting_npp->getName());
        event->getPeer()->sendPacket(success, true/*reliable*/);
        delete success;
    }
#endif
}   // writePlayerReport

//-----------------------------------------------------------------------------
/** Find out the public IP server or poll STK server asynchronously. */
void ServerLobby::asynchronousUpdate()
{
    if (m_rs_state.load() == RS_ASYNC_RESET)
    {
        resetVotingTime();
        resetServer();
        m_rs_state.store(RS_NONE);
    }

#ifdef ENABLE_SQLITE3
    pollDatabase();
#endif

    // Check if server owner has left
    updateServerOwner();

    if (ServerConfig::m_ranked && m_state.load() == WAITING_FOR_START_GAME)
        clearDisconnectedRankedPlayer();

    if (allowJoinedPlayersWaiting() /*|| (m_game_setup->isGrandPrix() &&
        m_state.load() == WAITING_FOR_START_GAME)*/)
    {
        // Only poll the STK server if server has been registered.
        if (m_server_id_online.load() != 0 &&
            m_state.load() != REGISTER_SELF_ADDRESS)
            checkIncomingConnectionRequests();
        handlePendingConnection();
    }

    if (m_server_id_online.load() != 0 &&
        allowJoinedPlayersWaiting() &&
        StkTime::getMonoTimeMs() > m_last_unsuccess_poll_time &&
        StkTime::getMonoTimeMs() > m_last_success_poll_time.load() + 30000)
    {
        Log::warn("ServerLobby", "Trying auto server recovery.");
        // For auto server recovery wait 3 seconds for next try
        m_last_unsuccess_poll_time = StkTime::getMonoTimeMs() + 3000;
        registerServer();
    }

    switch (m_state.load())
    {
    case SET_PUBLIC_ADDRESS:
    {
        // In case of LAN we don't need our public address or register with the
        // STK server, so we can directly go to the accepting clients state.
        if (NetworkConfig::get()->isLAN())
        {
            m_state = WAITING_FOR_START_GAME;
            STKHost::get()->startListening();
            return;
        }
        auto ip_type = NetworkConfig::get()->getIPType();
        // Set the IPv6 address first for possible IPv6 only server
        if (isIPv6Socket() && ip_type >= NetworkConfig::IP_V6)
        {
            STKHost::get()->setPublicAddress(AF_INET6);
        }
        if (ip_type == NetworkConfig::IP_V4 ||
            ip_type == NetworkConfig::IP_DUAL_STACK)
        {
            STKHost::get()->setPublicAddress(AF_INET);
        }
        if (STKHost::get()->getPublicAddress().isUnset() &&
            STKHost::get()->getPublicIPv6Address().empty())
        {
            m_state = ERROR_LEAVE;
        }
        else
        {
            STKHost::get()->startListening();
            m_state = REGISTER_SELF_ADDRESS;
        }
        break;
    }
    case REGISTER_SELF_ADDRESS:
    {
        if (m_game_setup->isGrandPrixStarted() || m_registered_for_once_only)
        {
            m_state = WAITING_FOR_START_GAME;
            break;
        }
        // Register this server with the STK server. This will block
        // this thread, because there is no need for the protocol manager
        // to react to any requests before the server is registered.
        if (m_server_registering.expired() && m_server_id_online.load() == 0)
            registerServer();

        if (m_server_registering.expired())
        {
            // Finished registering server
            if (m_server_id_online.load() != 0)
            {
                // For non grand prix server we only need to register to stk
                // addons once
                if (allowJoinedPlayersWaiting())
                    m_registered_for_once_only = true;
                m_state = WAITING_FOR_START_GAME;
            }
            else
            {
                // Exit now if failed to register to stk addons
                m_state = ERROR_LEAVE;
            }
        }
        break;
    }
    case WAITING_FOR_START_GAME:
    {
        if (ServerConfig::m_owner_less)
        {
            unsigned players = 0;
            STKHost::get()->updatePlayers(&players);
            if (((int)players >= ServerConfig::m_min_start_game_players ||
                m_game_setup->isGrandPrixStarted()) &&
                m_timeout.load() == std::numeric_limits<int64_t>::max())
            {
                m_timeout.store((int64_t)StkTime::getMonoTimeMs() +
                    (int64_t)
                    (ServerConfig::m_start_game_counter * 1000.0f));
            }
            else if ((int)players < ServerConfig::m_min_start_game_players &&
                !m_game_setup->isGrandPrixStarted())
            {
                resetPeersReady();
                if (m_timeout.load() != std::numeric_limits<int64_t>::max())
                    updatePlayerList();
                m_timeout.store(std::numeric_limits<int64_t>::max());
            }
            if ((!ServerConfig::m_soccer_tournament && !ServerConfig::m_race_tournament &&
                m_timeout.load() < (int64_t)StkTime::getMonoTimeMs()) ||
                (checkPeersReady(true/*ignore_ai_peer*/) &&
                (int)players >= ServerConfig::m_min_start_game_players))
            {
                resetPeersReady();
                startSelection();
                return;
            }
        }
        break;
    }
    case ERROR_LEAVE:
    {
        requestTerminate();
        m_state = EXITING;
        STKHost::get()->requestShutdown();
        break;
    }
    case WAIT_FOR_WORLD_LOADED:
    {
        // For WAIT_FOR_WORLD_LOADED and SELECTING make sure there are enough
        // players to start next game, otherwise exiting and let main thread
        // reset
        if (m_end_voting_period.load() == 0)
            return;

        unsigned player_in_game = 0;
        STKHost::get()->updatePlayers(&player_in_game);
        // Reset lobby will be done in main thread
        if ((player_in_game == 1 && ServerConfig::m_ranked) ||
            player_in_game == 0)
        {
            resetVotingTime();
            return;
        }

        // m_server_has_loaded_world is set by main thread with atomic write
        if (m_server_has_loaded_world.load() == false)
            return;
        if (!checkPeersReady(
            ServerConfig::m_ai_handling && m_ai_count == 0/*ignore_ai_peer*/))
            return;
        // Reset for next state usage
        resetPeersReady();
        configPeersStartTime();
        break;
    }
    case SELECTING:
    {
        if (m_end_voting_period.load() == 0)
            return;
        unsigned player_in_game = 0;
        STKHost::get()->updatePlayers(&player_in_game);
        if ((player_in_game == 1 && ServerConfig::m_ranked) ||
            player_in_game == 0)
        {
            resetVotingTime();
            return;
        }

        PeerVote winner_vote;
        m_winner_peer_id = std::numeric_limits<uint32_t>::max();
        bool go_on_race = false;
        if (ServerConfig::m_track_voting)
            go_on_race = handleAllVotes(&winner_vote, &m_winner_peer_id);
        else if (/*m_game_setup->isGrandPrixStarted() || */isVotingOver())
        {
            winner_vote = *m_default_vote;
            go_on_race = true;
        }
        if (go_on_race)
        {
            if (m_fixed_lap >= 0)
            {
                winner_vote.m_num_laps = m_fixed_lap;
                Log::info("ServerLobby", "Enforcing %d lap race", (int)m_fixed_lap);
            }
            *m_default_vote = winner_vote;
            m_item_seed = (uint32_t)StkTime::getTimeSinceEpoch();
            ItemManager::updateRandomSeed(m_item_seed);
            m_game_setup->setRace(winner_vote);
            std::string track_name = winner_vote.m_track_name;
            if (ServerConfig::m_soccer_tournament)
                m_tournament_arenas[m_tournament_game] = track_name;
            auto peers = STKHost::get()->getPeers();
            std::set<STKPeer*> bad_spectators;
            for (auto peer : peers)
            {
                if (peer->alwaysSpectate() &&
                    peer->getClientAssets().second.count(track_name) == 0)
                {
                    peer->setAlwaysSpectate(false);
                    peer->setWaitingForGame(true);
                    m_peers_ready.erase(peer);
                    bad_spectators.insert(peer.get());
                }
            }
            // if (!bad_spectators.empty())
            // {
            //     NetworkString* back_lobby = getNetworkString(2);
            //     back_lobby->setSynchronous(true);
            //     back_lobby->addUInt8(LE_BACK_LOBBY).addUInt8(BLR_NONE);
            //     STKHost::get()->sendPacketToAllPeersWith(
            //         [bad_spectators](STKPeer* peer) {
            //         return bad_spectators.find(peer) !=
            //         bad_spectators.end(); }, back_lobby, /*reliable*/true);
            //     delete back_lobby;
            // }
            bool has_always_on_spectators = false;
            auto players = STKHost::get()
                ->getPlayersForNewGame(&has_always_on_spectators);
            auto ai_instance = m_ai_peer.lock();
            if (supportsAI())
            {
                if (ai_instance)
                {
                    auto ai_profiles = ai_instance->getPlayerProfiles();
                    if (m_ai_count > 0)
                    {
                        ai_profiles.resize(m_ai_count);
                        players.insert(players.end(), ai_profiles.begin(),
                            ai_profiles.end());
                    }
                }
                else if (!m_ai_profiles.empty())
                {
                    players.insert(players.end(), m_ai_profiles.begin(),
                        m_ai_profiles.end());
                }
            }
            m_game_setup->sortPlayersForGrandPrix(players);
            m_game_setup->sortPlayersForGame(players);
            for (unsigned i = 0; i < players.size(); i++)
            {
                std::shared_ptr<NetworkPlayerProfile>& player = players[i];
                std::shared_ptr<STKPeer> peer = player->getPeer();
                if (peer)
                    peer->clearAvailableKartIDs();
            }
            for (unsigned i = 0; i < players.size(); i++)
            {
                std::shared_ptr<NetworkPlayerProfile>& player = players[i];
                std::shared_ptr<STKPeer> peer = player->getPeer();
                if (peer)
                    peer->addAvailableKartID(i);
            }
            getHitCaptureLimit();

            // Add placeholder players for live join
            addLiveJoinPlaceholder(players);
            // If player chose random / hasn't chose any kart
            bool possible_gnu_enforcement =
                m_gnu_elimination && m_gnu_remained >= 0;
            for (unsigned i = 0; i < players.size(); i++)
            {
                if (players[i]->getKartName().empty())
                {
                    bool gnu_eliminated = possible_gnu_enforcement;
					std::string username = StringUtils::wideToUtf8(players[i]->getName());
                    if (gnu_eliminated)
                    {
                        if (std::find(m_gnu_participants.begin(),
                            m_gnu_participants.begin() + m_gnu_remained,
                            StringUtils::wideToUtf8(players[i]->getName())) !=
                            m_gnu_participants.begin() + m_gnu_remained)
                        {
                            gnu_eliminated = false;
                        }
                    }
                    if (gnu_eliminated)
                    {
                        players[i]->setKartName(m_gnu_kart);
                    }
					else if (m_set_kart.count(username))
					{
						players[i]->setKartName(m_set_kart[username]);
					}
                    else
                    {
                        RandomGenerator rg;
                        std::set<std::string>::iterator it =
                            m_available_kts.first.begin();
                        std::advance(it,
                            rg.get((int)m_available_kts.first.size()));
                        players[i]->setKartName(*it);
                    }
                }
            }

            NetworkString* load_world_message = getLoadWorldMessage(players,
                false/*live_join*/);
            m_game_setup->setHitCaptureTime(m_battle_hit_capture_limit,
                m_battle_time_limit);
            uint16_t flag_return_time = (uint16_t)stk_config->time2Ticks(
                ServerConfig::m_flag_return_timeout);
            RaceManager::get()->setFlagReturnTicks(flag_return_time);
            uint16_t flag_deactivated_time = (uint16_t)stk_config->time2Ticks(
                ServerConfig::m_flag_deactivated_time);
            RaceManager::get()->setFlagDeactivatedTicks(flag_deactivated_time);
            configRemoteKart(players, 0);

            // Reset for next state usage
            resetPeersReady();

            m_state = LOAD_WORLD;
            sendMessageToPeers(load_world_message);
            // updatePlayerList so the in lobby players (if any) can see always
            // spectators join the game
            if (has_always_on_spectators || !bad_spectators.empty())
                updatePlayerList();
            delete load_world_message;
        }
        break;
    }
    default:
        break;
    }

}   // asynchronousUpdate

//-----------------------------------------------------------------------------
void ServerLobby::encodePlayers(BareNetworkString* bns,
        std::vector<std::shared_ptr<NetworkPlayerProfile> >& players) const
{
    bns->addUInt8((uint8_t)players.size());
    for (unsigned i = 0; i < players.size(); i++)
    {
        std::shared_ptr<NetworkPlayerProfile>& player = players[i];
        bns->encodeString(player->getName())
            .addUInt32(player->getHostId())
            .addFloat(player->getDefaultKartColor())
            .addUInt32(player->getOnlineId())
            .addUInt8(player->getHandicap())
            .addUInt8(player->getLocalPlayerId())
            .addUInt8(
            RaceManager::get()->teamEnabled() ? player->getTeam() : KART_TEAM_NONE)
            .encodeString(player->getCountryCode());
        bns->encodeString(player->getKartName());
    }
}   // encodePlayers

//-----------------------------------------------------------------------------
NetworkString* ServerLobby::getLoadWorldMessage(
    std::vector<std::shared_ptr<NetworkPlayerProfile> >& players,
    bool live_join) const
{
    NetworkString* load_world_message = getNetworkString();
    load_world_message->setSynchronous(true);
    load_world_message->addUInt8(LE_LOAD_WORLD);
    load_world_message->addUInt32(m_winner_peer_id);
    m_default_vote->encode(load_world_message);
    load_world_message->addUInt8(live_join ? 1 : 0);
    encodePlayers(load_world_message, players);
    load_world_message->addUInt32(m_item_seed);
    if (RaceManager::get()->isBattleMode())
    {
        load_world_message->addUInt32(m_battle_hit_capture_limit)
            .addFloat(m_battle_time_limit);
        uint16_t flag_return_time = (uint16_t)stk_config->time2Ticks(
            ServerConfig::m_flag_return_timeout);
        load_world_message->addUInt16(flag_return_time);
        uint16_t flag_deactivated_time = (uint16_t)stk_config->time2Ticks(
            ServerConfig::m_flag_deactivated_time);
        load_world_message->addUInt16(flag_deactivated_time);
    }
    return load_world_message;
}   // getLoadWorldMessage

//-----------------------------------------------------------------------------
/** Returns true if server can be live joined or spectating
 */
bool ServerLobby::canLiveJoinNow() const
{
    bool live_join = ServerConfig::m_live_players && worldIsActive();
    if (!live_join)
        return false;
    if (RaceManager::get()->modeHasLaps())
    {
        // No spectate when fastest kart is nearly finish, because if there
        // is endcontroller the spectating remote may not be knowing this
        // on time
        LinearWorld* w = dynamic_cast<LinearWorld*>(World::getWorld());
        if (!w)
            return false;
        AbstractKart* fastest_kart = NULL;
        for (unsigned i = 0; i < w->getNumKarts(); i++)
        {
            fastest_kart = w->getKartAtPosition(i + 1);
            if (fastest_kart && !fastest_kart->isEliminated())
                break;
        }
        if (!fastest_kart)
            return false;
        float progress = w->getOverallDistance(
            fastest_kart->getWorldKartId()) /
            (Track::getCurrentTrack()->getTrackLength() *
            (float)RaceManager::get()->getNumLaps());
        if (progress > 0.9f)
            return false;
    }
    return live_join;
}   // canLiveJoinNow

//-----------------------------------------------------------------------------
/** Returns true if world is active for clients to live join, spectate or
 *  going back to lobby live
 */
bool ServerLobby::worldIsActive() const
{
    return World::getWorld() && RaceEventManager::get()->isRunning() &&
        !RaceEventManager::get()->isRaceOver() &&
        World::getWorld()->getPhase() == WorldStatus::RACE_PHASE;
}   // worldIsActive

//-----------------------------------------------------------------------------
/** \ref STKPeer peer will be reset back to the lobby with reason
 *  \ref BackLobbyReason blr
 */
void ServerLobby::rejectLiveJoin(STKPeer* peer, BackLobbyReason blr)
{
    NetworkString* reset = getNetworkString(2);
    reset->setSynchronous(true);
    reset->addUInt8(LE_BACK_LOBBY).addUInt8(blr);
    peer->sendPacket(reset, /*reliable*/true);
    delete reset;
    updatePlayerList();
    NetworkString* server_info = getNetworkString();
    server_info->setSynchronous(true);
    server_info->addUInt8(LE_SERVER_INFO);
    m_game_setup->addServerInfo(server_info);
    peer->sendPacket(server_info, /*reliable*/true);
    delete server_info;
}   // rejectLiveJoin

//-----------------------------------------------------------------------------
/** This message is like kartSelectionRequested, but it will send the peer
 *  load world message if he can join the current started game.
 */
void rem_gamescore3(std::string player_name, double phase)
{
    std::string ringdrossel;
    if(ServerConfig::m_rank_1vs1 || ServerConfig::m_rank_1vs1_2 || ServerConfig::m_rank_1vs1_3) return;//ringdrossel="python3 update_list.py "+player_name+" leftgame "+std::to_string(phase)+" 1vs1";
    else ringdrossel="python3 update_list.py "+player_name+" leftgame "+std::to_string(phase)+" 3vs3";
    system(ringdrossel.c_str());
}

void ServerLobby::liveJoinRequest(Event* event)
{
    STKPeer* peer = event->getPeer();
    const NetworkString& data = event->data();

    if (!canLiveJoinNow())
    {
        rejectLiveJoin(peer, BLR_NO_GAME_FOR_LIVE_JOIN);
        return;
    }
    bool spectator = data.getUInt8() == 1;
    if (RaceManager::get()->modeHasLaps() && !spectator)
    {
        // No live join for linear race
        rejectLiveJoin(peer, BLR_NO_GAME_FOR_LIVE_JOIN);
        return;
    }

    peer->clearAvailableKartIDs();
    if (!spectator)
    {
        setPlayerKarts(data, peer);

        std::vector<int> used_id;
        for (unsigned i = 0; i < peer->getPlayerProfiles().size(); i++)
        {
            int id = getReservedId(peer->getPlayerProfiles()[i], i);
            if (id == -1)
                break;
            used_id.push_back(id);
        }

		// Check number of players in the red and in the blue team
		int red = 0, blue = 0;
		for (auto &player_peer_wp : m_peers_ready)
		{
			auto player_peer_sp = player_peer_wp.first.lock();
			if (player_peer_sp->isSpectator()) continue;
			for (auto &player : player_peer_sp->getPlayerProfiles())
			{
				if (player->getTeam() == KART_TEAM_RED) red++;
				else if (player->getTeam() == KART_TEAM_BLUE) blue++;
			}
		}
		for (auto &player : peer->getPlayerProfiles())
		{
			if (player->getTeam() == KART_TEAM_RED) red++;
			else if (player->getTeam() == KART_TEAM_BLUE) blue++;
		}

		// Reject live join if teams are unbalanced (only red or only blue players)
		if (/*!ServerConfig::m_owner_less &&*/ ServerConfig::m_team_choosing &&
			!ServerConfig::m_free_teams && RaceManager::get()->teamEnabled())
		{
			if (red + blue > 1 && (red == 0 || blue == 0))
			{
				for (unsigned i = 0; i < peer->getPlayerProfiles().size(); i++)
					peer->getPlayerProfiles()[i]->setKartName("");
				for (unsigned i = 0; i < used_id.size(); i++)
				{
					RemoteKartInfo& rki = RaceManager::get()->getKartInfo(used_id[i]);
					rki.makeReserved();
				}

				Log::warn("ServerLobby", "Bad team choosing (live join).");
				NetworkString* bt = getNetworkString();
				bt->setSynchronous(true);
				bt->addUInt8(LE_BAD_TEAM);
				peer->sendPacket(bt, true/*reliable*/);
				delete bt;
				rejectLiveJoin(peer, BLR_NONE);
				return;
			}
		}

		// Reject live join if player limit is reached
		bool queuePlayerLimitReached = m_player_queue_limit > 0 && red + blue > m_player_queue_limit;
        if (used_id.size() != peer->getPlayerProfiles().size() || queuePlayerLimitReached)
        {
            for (unsigned i = 0; i < peer->getPlayerProfiles().size(); i++)
                peer->getPlayerProfiles()[i]->setKartName("");
            for (unsigned i = 0; i < used_id.size(); i++)
            {
                RemoteKartInfo& rki = RaceManager::get()->getKartInfo(used_id[i]);
                rki.makeReserved();
            }
            Log::info("ServerLobby", "Too many players (%d) try to live join",
                (int)peer->getPlayerProfiles().size());
            rejectLiveJoin(peer, BLR_NO_PLACE_FOR_LIVE_JOIN);
            return;
        }

        for (int id : used_id)
        {
            Log::info("ServerLobby", "%s live joining with reserved kart id %d.",
                peer->getAddress().toString().c_str(), id);
            peer->addAvailableKartID(id);
            if (ServerConfig::m_save_goals)
            {
                    std::string username = StringUtils::wideToUtf8(peer->getPlayerProfiles()[0]->getName());
                    double phase = 0.0;
                    if (RaceManager::get()->hasTimeTarget())
                    {
                        phase = -1.0 + (RaceManager::get()->getTimeTarget() - World::getWorld()->getTime())/RaceManager::get()->getTimeTarget();
                    }
                    else
                    {
                        int red_scorers_count = 0; int blue_scorers_count = 0;
                        SoccerWorld *sw = dynamic_cast<SoccerWorld*>(World::getWorld());
                        if (sw)
                        {
                            red_scorers_count = sw->get_red_scorers_count();
                            blue_scorers_count = sw->get_blue_scorers_count();
                        }
                        phase = -1.0 + 1.0*std::max(red_scorers_count, blue_scorers_count)/RaceManager::get()->getMaxGoal();
                        std::string message = "red_scorers_cnt=" + std::to_string(red_scorers_count) + " / blue_scorers_cnt" + std::to_string(blue_scorers_count);
                        message += " / max_goll=" + std::to_string(RaceManager::get()->getMaxGoal());
                        Log::info("ServerLobby", message.c_str());
                    }
                    std::string message = "phase=" + std::to_string(phase);
                    Log::info("ServerLobby", message.c_str());
                    rem_gamescore3(username,phase);
            }
            if (ServerConfig::m_super_tournament && ServerConfig::m_count_supertournament_game)
            {
                std::string username = StringUtils::wideToUtf8(peer->getPlayerProfiles()[0]->getName());
                std::string singdrossel;
                std::string redname=ServerConfig::m_red_team_name;
                std::string bluename=ServerConfig::m_blue_team_name;
                if(m_tournament_red_players.count(username) > 0) singdrossel="python3 supertournament_addcurrentplayer.py "+username+" "+redname;
                else singdrossel="python3 supertournament_addcurrentplayer.py "+username+" "+bluename;
                system(singdrossel.c_str());
            }
        }
    }
    else
    {
        Log::info("ServerLobby", "%s spectating now.",
            peer->getAddress().toString().c_str());
    }

    std::vector<std::shared_ptr<NetworkPlayerProfile> > players =
        getLivePlayers();
    NetworkString* load_world_message = getLoadWorldMessage(players,
        true/*live_join*/);
    peer->sendPacket(load_world_message, true/*reliable*/);
    delete load_world_message;
    peer->updateLastActivity();
}   // liveJoinRequest

//-----------------------------------------------------------------------------
/** Get a list of current ingame players for live join or spectate.
 */
std::vector<std::shared_ptr<NetworkPlayerProfile> >
                                            ServerLobby::getLivePlayers() const
{
    std::vector<std::shared_ptr<NetworkPlayerProfile> > players;
    for (unsigned i = 0; i < RaceManager::get()->getNumPlayers(); i++)
    {
        const RemoteKartInfo& rki = RaceManager::get()->getKartInfo(i);
        std::shared_ptr<NetworkPlayerProfile> player =
            rki.getNetworkPlayerProfile().lock();
        if (!player)
        {
            if (RaceManager::get()->modeHasLaps())
            {
                player = std::make_shared<NetworkPlayerProfile>(
                    nullptr, rki.getPlayerName(),
                    std::numeric_limits<uint32_t>::max(),
                    rki.getDefaultKartColor(),
                    rki.getOnlineId(), rki.getHandicap(),
                    rki.getLocalPlayerId(), KART_TEAM_NONE,
                    rki.getCountryCode());
                player->setKartName(rki.getKartName());
            }
            else
            {
                player = NetworkPlayerProfile::getReservedProfile(
                    RaceManager::get()->getMinorMode() ==
                    RaceManager::MINOR_MODE_FREE_FOR_ALL ?
                    KART_TEAM_NONE : rki.getKartTeam());
            }
        }
        players.push_back(player);
    }
    return players;
}   // getLivePlayers

//-----------------------------------------------------------------------------
/** Decide where to put the live join player depends on his team and game mode.
 */
int ServerLobby::getReservedId(std::shared_ptr<NetworkPlayerProfile>& p,
                               unsigned local_id) const
{
    const bool is_ffa =
        RaceManager::get()->getMinorMode() == RaceManager::MINOR_MODE_FREE_FOR_ALL;
    int red_count = 0;
    int blue_count = 0;
    for (unsigned i = 0; i < RaceManager::get()->getNumPlayers(); i++)
    {
        RemoteKartInfo& rki = RaceManager::get()->getKartInfo(i);
        if (rki.isReserved())
            continue;
        bool disconnected = rki.disconnected();
        if (RaceManager::get()->getKartInfo(i).getKartTeam() == KART_TEAM_RED &&
            !disconnected)
            red_count++;
        else if (RaceManager::get()->getKartInfo(i).getKartTeam() ==
            KART_TEAM_BLUE && !disconnected)
            blue_count++;
    }
    KartTeam target_team = red_count > blue_count ? KART_TEAM_BLUE :
        KART_TEAM_RED;

    for (unsigned i = 0; i < RaceManager::get()->getNumPlayers(); i++)
    {
        RemoteKartInfo& rki = RaceManager::get()->getKartInfo(i);
        std::shared_ptr<NetworkPlayerProfile> player =
            rki.getNetworkPlayerProfile().lock();
        if (!player)
        {
            if (is_ffa)
            {
                rki.copyFrom(p, local_id);
                return i;
            }
            if (ServerConfig::m_team_choosing)
            {
                if ((p->getTeam() == KART_TEAM_RED &&
                    rki.getKartTeam() == KART_TEAM_RED) ||
                    (p->getTeam() == KART_TEAM_BLUE &&
                    rki.getKartTeam() == KART_TEAM_BLUE))
                {
                    rki.copyFrom(p, local_id);
                    return i;
                }
            }
            else
            {
                if (rki.getKartTeam() == target_team)
                {
                    p->setTeam(target_team);
                    rki.copyFrom(p, local_id);
                    return i;
                }
            }
        }
    }
    return -1;
}   // getReservedId

//-----------------------------------------------------------------------------
/** Finally put the kart in the world and inform client the current world
 *  status, (including current confirmed item state, kart scores...)
 */
void ServerLobby::finishedLoadingLiveJoinClient(Event* event)
{
    std::shared_ptr<STKPeer> peer = event->getPeerSP();
    if (!canLiveJoinNow())
    {
        rejectLiveJoin(peer.get(), BLR_NO_GAME_FOR_LIVE_JOIN);
        return;
    }
    bool live_joined_in_time = true;
    for (const int id : peer->getAvailableKartIDs())
    {
        const RemoteKartInfo& rki = RaceManager::get()->getKartInfo(id);
        if (rki.isReserved())
        {
            live_joined_in_time = false;
            break;
        }
    }
    if (!live_joined_in_time)
    {
        Log::warn("ServerLobby", "%s can't live-join in time.",
            peer->getAddress().toString().c_str());
        rejectLiveJoin(peer.get(), BLR_NO_GAME_FOR_LIVE_JOIN);
        return;
    }
    World* w = World::getWorld();
    assert(w);

    uint64_t live_join_start_time = STKHost::get()->getNetworkTimer();

    // Instead of using getTicksSinceStart we caculate the current world ticks
    // only from network timer, because if the server hangs in between the
    // world ticks may not be up to date
    // 2000 is the time for ready set, remove 3 ticks after for minor
    // correction (make it more looks like getTicksSinceStart if server has no
    // hang
    int cur_world_ticks = stk_config->time2Ticks(
        (live_join_start_time - m_server_started_at - 2000) / 1000.f) - 3;
    // Give 3 seconds for all peers to get new kart info
    m_last_live_join_util_ticks =
        cur_world_ticks + stk_config->time2Ticks(3.0f);
    live_join_start_time -= m_server_delay;
    live_join_start_time += 3000;

    bool spectator = false;
    for (const int id : peer->getAvailableKartIDs())
    {
        World::getWorld()->addReservedKart(id);
        const RemoteKartInfo& rki = RaceManager::get()->getKartInfo(id);
        addLiveJoiningKart(id, rki, m_last_live_join_util_ticks);
        Log::info("ServerLobby", "%s succeeded live-joining with kart id %d.",
            peer->getAddress().toString().c_str(), id);
    }
    if (peer->getAvailableKartIDs().empty())
    {
        Log::info("ServerLobby", "%s spectating succeeded.",
            peer->getAddress().toString().c_str());
        spectator = true;
    }

    const uint8_t cc = (uint8_t)Track::getCurrentTrack()->getCheckManager()->getCheckStructureCount();
    NetworkString* ns = getNetworkString(10);
    ns->setSynchronous(true);
    ns->addUInt8(LE_LIVE_JOIN_ACK).addUInt64(m_client_starting_time)
        .addUInt8(cc).addUInt64(live_join_start_time)
        .addUInt32(m_last_live_join_util_ticks);

    NetworkItemManager* nim = dynamic_cast<NetworkItemManager*>
        (Track::getCurrentTrack()->getItemManager());
    assert(nim);
    nim->saveCompleteState(ns);
    nim->addLiveJoinPeer(peer);

    w->saveCompleteState(ns, peer.get());
    if (RaceManager::get()->supportsLiveJoining())
    {
        // Only needed in non-racing mode as no need players can added after
        // starting of race
        std::vector<std::shared_ptr<NetworkPlayerProfile> > players =
            getLivePlayers();
        encodePlayers(ns, players);
    }

    m_peers_ready[peer] = false;
    peer->setWaitingForGame(false);
    peer->setSpectator(spectator);

    peer->sendPacket(ns, true/*reliable*/);
    delete ns;
    updatePlayerList();
    peer->updateLastActivity();
}   // finishedLoadingLiveJoinClient

//-----------------------------------------------------------------------------
/** Simple finite state machine.  Once this
 *  is known, register the server and its address with the stk server so that
 *  client can find it.
 */
void ServerLobby::update(int ticks)
{
    World* w = World::getWorld();
    bool world_started = m_state.load() >= WAIT_FOR_WORLD_LOADED &&
        m_state.load() <= RACING && m_server_has_loaded_world.load();
    bool all_players_in_world_disconnected = (w != NULL && world_started);
    int sec = ServerConfig::m_kick_idle_player_seconds;
    if (world_started)
    {
        for (unsigned i = 0; i < RaceManager::get()->getNumPlayers(); i++)
        {
            RemoteKartInfo& rki = RaceManager::get()->getKartInfo(i);
            std::shared_ptr<NetworkPlayerProfile> player =
                rki.getNetworkPlayerProfile().lock();
            if (player)
            {
                if (w)
                    all_players_in_world_disconnected = false;
            }
            else
                continue;
            auto peer = player->getPeer();
            if (!peer)
                continue;

            if (peer->idleForSeconds() > 60 && w &&
                w->getKart(i)->isEliminated())
            {
                // Remove loading world too long (60 seconds) live join peer
                Log::info("ServerLobby", "%s hasn't live-joined within"
                    " 60 seconds, remove it.",
                    peer->getAddress().toString().c_str());
                rki.makeReserved();
                continue;
            }
            if (!peer->isAIPeer() &&
                sec > 0 && peer->idleForSeconds() > sec &&
                !peer->isDisconnected() && NetworkConfig::get()->isWAN())
            {
                if (w && w->getKart(i)->hasFinishedRace())
                    continue;
                // Don't kick in game GUI server host so he can idle in game
                if (m_process_type == PT_CHILD &&
                    peer->getHostId() == m_client_server_host_id.load())
                    continue;
                Log::info("ServerLobby", "%s %s has been idle for more than"
                    " %d seconds, kick.",
                    peer->getAddress().toString().c_str(),
                    StringUtils::wideToUtf8(rki.getPlayerName()).c_str(), sec);
                peer->kick();
            }
        }
    }
    if (w)
        setGameStartedProgress(w->getGameStartedProgress());
    else
        resetGameStartedProgress();

    if (w && w->getPhase() == World::RACE_PHASE)
    {
        storePlayingTrack(RaceManager::get()->getTrackName());
    }
    else
        storePlayingTrack("");

    // Reset server to initial state if no more connected players
    if (m_rs_state.load() == RS_WAITING)
    {
        if ((RaceEventManager::get() &&
            !RaceEventManager::get()->protocolStopped()) ||
            !GameProtocol::emptyInstance())
            return;

        exitGameState();
        m_rs_state.store(RS_ASYNC_RESET);
    }

    STKHost::get()->updatePlayers();
    if (m_rs_state.load() == RS_NONE &&
        (m_state.load() > WAITING_FOR_START_GAME/* ||
        m_game_setup->isGrandPrixStarted()*/) &&
        (STKHost::get()->getPlayersInGame() == 0 ||
        all_players_in_world_disconnected))
    {
        if (RaceEventManager::get() &&
            RaceEventManager::get()->isRunning())
        {
            // Send a notification to all players who may have start live join
            // or spectate to go back to lobby
            NetworkString* back_to_lobby = getNetworkString(2);
            back_to_lobby->setSynchronous(true);
            back_to_lobby->addUInt8(LE_BACK_LOBBY).addUInt8(BLR_NONE);
            sendMessageToPeersInServer(back_to_lobby, /*reliable*/true);
            delete back_to_lobby;

            RaceEventManager::get()->stop();
            RaceEventManager::get()->getProtocol()->requestTerminate();
            GameProtocol::lock()->requestTerminate();
        }
        else if (auto ai = m_ai_peer.lock())
        {
            // Reset AI peer for empty server, which will delete world
            NetworkString* back_to_lobby = getNetworkString(2);
            back_to_lobby->setSynchronous(true);
            back_to_lobby->addUInt8(LE_BACK_LOBBY).addUInt8(BLR_NONE);
            ai->sendPacket(back_to_lobby, /*reliable*/true);
            delete back_to_lobby;
        }
        if (all_players_in_world_disconnected)
            m_game_setup->cancelOneRace();
        resetVotingTime();
        // m_game_setup->cancelOneRace();
        //m_game_setup->stopGrandPrix();
        m_rs_state.store(RS_WAITING);
        return;
    }

    if (m_rs_state.load() != RS_NONE)
        return;

    // Reset for ranked server if in kart / track selection has only 1 player
	bool ranked = ServerConfig::m_ranked || ServerConfig::m_rank_1vs1 || ServerConfig::m_rank_1vs1_2 || ServerConfig::m_rank_1vs1_3;
    if (ranked &&
        m_state.load() == SELECTING &&
        STKHost::get()->getPlayersInGame() == 1)
    {
        NetworkString* back_lobby = getNetworkString(2);
        back_lobby->setSynchronous(true);
        back_lobby->addUInt8(LE_BACK_LOBBY)
            .addUInt8(BLR_ONE_PLAYER_IN_RANKED_MATCH);
        sendMessageToPeers(back_lobby, /*reliable*/true);
        delete back_lobby;
        resetVotingTime();
        // m_game_setup->cancelOneRace();
        //m_game_setup->stopGrandPrix();
        m_rs_state.store(RS_ASYNC_RESET);
    }

    handlePlayerDisconnection();

    switch (m_state.load())
    {
    case SET_PUBLIC_ADDRESS:
    case REGISTER_SELF_ADDRESS:
    case WAITING_FOR_START_GAME:
		if (m_player_queue_limit > 0 && m_player_queue_rotable)
		{
			rotatePlayerQueue();
			m_player_queue_rotable = false;
		}
		break;
    case WAIT_FOR_WORLD_LOADED:
    case WAIT_FOR_RACE_STARTED:
    {
        // Waiting for asynchronousUpdate
        break;
    }
    case SELECTING:
        // The function playerTrackVote will trigger the next state
        // once all track votes have been received.
        break;
    case LOAD_WORLD:
        Log::info("ServerLobbyRoom", "Starting the race loading.");
        // This will create the world instance, i.e. load track and karts
		init1vs1Ranking();
		m_player_queue_rotable = true;
        loadWorld();
        updateWorldSettings();
        m_state = WAIT_FOR_WORLD_LOADED;
        break;
    case RACING:
        if (World::getWorld() && RaceEventManager::get() &&
            RaceEventManager::get()->isRunning())
        {
            checkRaceFinished();
        }
        break;
    case WAIT_FOR_RACE_STOPPED:
        if (!RaceEventManager::get()->protocolStopped() ||
            !GameProtocol::emptyInstance())
            return;

        // This will go back to lobby in server (and exit the current race)
        exitGameState();
		// Enable all karts again
		m_set_kart.clear();
        // Reset for next state usage
        resetPeersReady();
        // Set the delay before the server forces all clients to exit the race
        // result screen and go back to the lobby
        m_timeout.store((int64_t)StkTime::getMonoTimeMs() + 15000);
        m_state = RESULT_DISPLAY;
        sendMessageToPeers(m_result_ns, /*reliable*/ true);
        if (ServerConfig::m_rank_1vs1)
        {
            system("python3 update_elo.py 1vs1");
        }
        if (ServerConfig::m_rank_1vs1_2)
        {
            system("python3 update_elo.py 1vs1_2");
        }
        if (ServerConfig::m_rank_1vs1_3)
        {
            system("python3 update_elo.py 1vs1_3");
        }
        if (ServerConfig::m_save_goals)
        {
            if (ServerConfig::m_rank_1vs1 || ServerConfig::m_rank_1vs1_2 || ServerConfig::m_rank_1vs1_3) system("python3 update_wiki.py 1vs1");
            if (ServerConfig::m_rank_1vs1) system("python3 update_wiki.py 1vs1");
            else system("python3 update_wiki.py 3vs3");
        }
        if (ServerConfig::m_super_tournament && ServerConfig::m_count_supertournament_game && !(ServerConfig::m_skip_end))
        {
            std::string redname=ServerConfig::m_red_team_name;
            std::string bluename=ServerConfig::m_blue_team_name;
			std::string singdrossel="python3 supertournament_gameresult.py "+redname+' '+bluename;
            system(singdrossel.c_str());
        }
        Log::info("ServerLobby", "End of game message sent");
        break;
    case RESULT_DISPLAY:
        if (checkPeersReady(true/*ignore_ai_peer*/) ||
            (int64_t)StkTime::getMonoTimeMs() > m_timeout.load())
        {
            // Send a notification to all clients to exit
            // the race result screen
            NetworkString* back_to_lobby = getNetworkString(2);
            back_to_lobby->setSynchronous(true);
            back_to_lobby->addUInt8(LE_BACK_LOBBY).addUInt8(BLR_NONE);
            sendMessageToPeersInServer(back_to_lobby, /*reliable*/true);
            delete back_to_lobby;
            m_rs_state.store(RS_ASYNC_RESET);
        }
        break;
    case ERROR_LEAVE:
    case EXITING:
        break;
    }
}   // update

//-----------------------------------------------------------------------------
/** Register this server (i.e. its public address) with the STK server
 *  so that clients can find it. It blocks till a response from the
 *  stk server is received (this function is executed from the 
 *  ProtocolManager thread). The information about this client is added
 *  to the table 'server'.
 */
void ServerLobby::registerServer()
{
    // ========================================================================
    class RegisterServerRequest : public Online::XMLRequest
    {
    private:
        std::weak_ptr<ServerLobby> m_server_lobby;
    protected:
        virtual void afterOperation()
        {
            Online::XMLRequest::afterOperation();
            const XMLNode* result = getXMLData();
            std::string rec_success;
            auto sl = m_server_lobby.lock();
            if (!sl)
                return;

            if (result->get("success", &rec_success) &&
                rec_success == "yes")
            {
                const XMLNode* server = result->getNode("server");
                assert(server);
                const XMLNode* server_info = server->getNode("server-info");
                assert(server_info);
                unsigned server_id_online = 0;
                server_info->get("id", &server_id_online);
                assert(server_id_online != 0);
                bool is_official = false;
                server_info->get("official", &is_official);
                if (!is_official && ServerConfig::m_ranked)
                {
                    Log::fatal("ServerLobby", "You don't have permission to "
                        "host a ranked server.");
                }
                Log::info("ServerLobby",
                    "Server %d is now online.", server_id_online);
                sl->m_server_id_online.store(server_id_online);
                sl->m_last_success_poll_time.store(StkTime::getMonoTimeMs());
                return;
            }
            Log::error("ServerLobby", "%s",
                StringUtils::wideToUtf8(getInfo()).c_str());
        }
    public:
        RegisterServerRequest(std::shared_ptr<ServerLobby> sl)
        : XMLRequest(Online::RequestManager::HTTP_MAX_PRIORITY),
        m_server_lobby(sl) {}
    };   // RegisterServerRequest

    auto request = std::make_shared<RegisterServerRequest>(
        std::dynamic_pointer_cast<ServerLobby>(shared_from_this()));
    NetworkConfig::get()->setServerDetails(request, "create");
    const SocketAddress& addr = STKHost::get()->getPublicAddress();
    request->addParameter("address",      addr.getIP()        );
    request->addParameter("port",         addr.getPort()      );
    request->addParameter("private_port",
                                    STKHost::get()->getPrivatePort()      );
    request->addParameter("name", m_game_setup->getServerNameUtf8());
    request->addParameter("max_players", ServerConfig::m_server_max_players);
    int difficulty = m_difficulty.load();
    request->addParameter("difficulty", difficulty);
    int game_mode = m_game_mode.load();
    request->addParameter("game_mode", game_mode);
    const std::string& pw = ServerConfig::m_private_server_password;
    request->addParameter("password", (unsigned)(!pw.empty()));
    request->addParameter("version", (unsigned)ServerConfig::m_server_version);

    bool ipv6_only = addr.isUnset();
    if (!ipv6_only)
    {
        Log::info("ServerLobby", "Public IPv4 server address %s",
            addr.toString().c_str());
    }
    if (!STKHost::get()->getPublicIPv6Address().empty())
    {
        request->addParameter("address_ipv6",
            STKHost::get()->getPublicIPv6Address());
        Log::info("ServerLobby", "Public IPv6 server address %s",
            STKHost::get()->getPublicIPv6Address().c_str());
    }
    request->queue();
    m_server_registering = request;
}   // registerServer

//-----------------------------------------------------------------------------
/** Unregister this server (i.e. its public address) with the STK server,
 *  currently when karts enter kart selection screen it will be done or quit
 *  stk.
 */
void ServerLobby::unregisterServer(bool now, std::weak_ptr<ServerLobby> sl)
{
    // ========================================================================
    class UnRegisterServerRequest : public Online::XMLRequest
    {
    private:
        std::weak_ptr<ServerLobby> m_server_lobby;
    protected:
        virtual void afterOperation()
        {
            Online::XMLRequest::afterOperation();
            const XMLNode* result = getXMLData();
            std::string rec_success;

            if (result->get("success", &rec_success) &&
                rec_success == "yes")
            {
                // Clear the server online for next register
                // For grand prix server
                if (auto sl = m_server_lobby.lock())
                    sl->m_server_id_online.store(0);
                return;
            }
            Log::error("ServerLobby", "%s",
                StringUtils::wideToUtf8(getInfo()).c_str());
        }
    public:
        UnRegisterServerRequest(std::weak_ptr<ServerLobby> sl)
        : XMLRequest(Online::RequestManager::HTTP_MAX_PRIORITY),
        m_server_lobby(sl) {}
    };   // UnRegisterServerRequest
    auto request = std::make_shared<UnRegisterServerRequest>(sl);
    NetworkConfig::get()->setServerDetails(request, "stop");

    const SocketAddress& addr = STKHost::get()->getPublicAddress();
    request->addParameter("address", addr.getIP());
    request->addParameter("port", addr.getPort());
    bool ipv6_only = addr.isUnset();
    if (!ipv6_only)
    {
        Log::info("ServerLobby", "Unregister server address %s",
            addr.toString().c_str());
    }
    else
    {
        Log::info("ServerLobby", "Unregister server address %s",
            STKHost::get()->getValidPublicAddress().c_str());
    }

    // No need to check for result as server will be auto-cleared anyway
    // when no polling is done
    if (now)
    {
        request->executeNow();
    }
    else
        request->queue();

}   // unregisterServer

//-----------------------------------------------------------------------------
/** Instructs all clients to start the kart selection. If event is NULL,
 *  the command comes from the owner less server.
 */
void ServerLobby::startSelection(const Event *event)
{
    bool need_to_update = false;
    if (event != NULL)
    {
        if (m_state != WAITING_FOR_START_GAME)
        {
            Log::warn("ServerLobby",
                "Received startSelection while being in state %d.",
                m_state.load());
            return;
        }
        if (ServerConfig::m_sleeping_server) {
            Log::warn("ServerLobby",
                "An attempt to start a race on a sleeping server. Lol.");
            return;
        }
        auto peer = event->getPeerSP();
        if (ServerConfig::m_owner_less)
        {
            if (!m_allowed_to_start) {
                std::string msg = "Starting the game is forbidden by server owner";
                sendStringToPeer(msg, peer);
                return;
            }
            if (!canRace(peer))
            {
                std::string msg = "You cannot play so pressing ready has no action";
                sendStringToPeer(msg, peer);
                return;
            }
            else
            {
                m_peers_ready.at(event->getPeerSP()) =
                    !m_peers_ready.at(event->getPeerSP());
                updatePlayerList();
                return;
            }
        }
        if (!m_allowed_to_start) {
            std::string msg = "Starting the game is forbidden by server owner";
            sendStringToPeer(msg, peer);
            return;
        }
        if (peer != m_server_owner.lock())
        {
            Log::warn("ServerLobby",
                "Client %d is not authorised to start selection.",
                event->getPeer()->getHostId());
            return;
        }
    } else {
        if (!m_allowed_to_start) {
            // Produce no log spam
            return;
        }
    }

    if (/*!ServerConfig::m_owner_less &&*/ ServerConfig::m_team_choosing &&
        !ServerConfig::m_free_teams && RaceManager::get()->teamEnabled())
    {
        //auto red_blue = STKHost::get()->getAllPlayersTeamInfo();
		//bool badTeams = (red_blue.first == 0 || red_blue.second == 0) && (red_blue.first + red_blue.second != 1);
        if (!teamsBalanced())
        {
            Log::warn("ServerLobby", "Bad team choosing.");
            if (event)
            {
                NetworkString* bt = getNetworkString();
                bt->setSynchronous(true);
                bt->addUInt8(LE_BAD_TEAM);
                event->getPeer()->sendPacket(bt, true/*reliable*/);
                delete bt;
            }
            return;
        }
    }

    // Remove karts / tracks from server that are not supported on all clients
    std::set<std::string> karts_erase, tracks_erase;
    auto peers = STKHost::get()->getPeers();
    std::set<STKPeer*> always_spectate_peers;
    bool has_peer_plays_game = false;
    int racing_players_count = 0;
    for (auto peer : peers)
    {
        if (!peer->isValidated() || peer->isWaitingForGame())
            continue;
        bool can_race = canRace(peer);
        if (!can_race)
        {
			if (ServerConfig::m_soccer_tournament || ServerConfig::m_race_tournament)
			{
				peer->setAlwaysSpectate(true);
			}
			if (!peer->alwaysSpectate())
			{
				peer->setWaitingForGame(true);
				m_peers_ready.erase(peer);
				need_to_update = true;
				continue;
			}
        }

        if (peer->alwaysSpectate())
        {
            always_spectate_peers.insert(peer.get());
            continue;
        }
        peer->eraseServerKarts(m_available_kts.first, karts_erase);
        peer->eraseServerTracks(m_available_kts.second, tracks_erase);
        if (!peer->isAIPeer())
            has_peer_plays_game = true;
        racing_players_count++;
    }
    m_default_always_spectate_peers = always_spectate_peers;

    // kimden thinks if someone wants to race he should disable spectating
    // // Disable always spectate peers if no players join the game
    if (!has_peer_plays_game)
    {
        Log::warn("ServerLobby",
            "An attempt to start a race while no one is able to race.");
        return;
        // for (STKPeer* peer : always_spectate_peers)
        //     peer->setAlwaysSpectate(false);
        // always_spectate_peers.clear();
    }
    else
    {
        // We make those always spectate peer waiting for game so it won't
        // be able to vote, this will be reset in STKHost::getPlayersForNewGame
        // This will also allow a correct number of in game players for max
        // arena players handling
        for (STKPeer* peer : always_spectate_peers)
            peer->setWaitingForGame(true);
    }

    // if (ServerConfig::m_soccer_tournament && !tournamentHasIcy(m_tournament_game))
    // {
    //     tracks_erase.insert("icy_soccer_field");
    // }

    for (const std::string& kart_erase : karts_erase)
    {
        m_available_kts.first.erase(kart_erase);
    }
    for (const std::string& track_erase : tracks_erase)
    {
        m_available_kts.second.erase(track_erase);
    }
    
    // The host can set a soccer field using the command /setfield
	if (m_set_field != "")
	{
		if (m_available_kts.second.count(m_set_field))
		{
			m_available_kts.second.clear();
			m_available_kts.second.insert(m_set_field);
		}
		else
		{
			m_available_kts.second.clear();
		}
		//m_available_kts.second.insert("icy_soccer_field");
		m_set_field = "";
	}

	if (m_gnu_elimination && m_gnu2_activated) 	// For gnu2 elimination, a specific selection of tracks is available
	{
		if (!(m_gnu2_initialized)) // initialize gnu2 elimination
		{
			// Number of selectable tracks must be player_count - 1
			int player_count = racing_players_count;
			int tracks_count = player_count - 1;
			if (tracks_count <= 0) tracks_count = 1;

			if (m_gnu2_available_tracks.size() < tracks_count)
				selectRandomTracks(m_gnu2_available_tracks, tracks_count);

			if (m_gnu2_available_tracks.size() > tracks_count)
			{
				if (ServerConfig::m_gnu2_random_tracks)
					selectRandomTracks(m_gnu2_available_tracks, tracks_count);
				else
					m_gnu2_available_tracks.erase(m_gnu2_available_tracks.begin() + tracks_count, m_gnu2_available_tracks.end());
			}

			m_gnu2_initialized = true;
		}

		m_available_kts.second.clear();
		for (std::string &track : m_gnu2_available_tracks)
			m_available_kts.second.insert(track);
	}
    
    if (ServerConfig::m_soccer_tournament)
	{
		if (!tournamentHasIcy(m_tournament_game))
		{
			tracks_erase.insert("icy_soccer_field");
		}

		if (!tournamentHasTournamentField(m_tournament_game))
		{
			tracks_erase.insert("addon_supertournament-field");
		}
		if (!tournamentHasGrass(m_tournament_game))
                {
                        tracks_erase.insert("addon_tournament-field");
                        tracks_erase.insert("soccer_field");
                        tracks_erase.insert("lasdunassoccer");
                }

		for (const std::string& kart_erase : karts_erase)
		{
			m_available_kts.first.erase(kart_erase);
		}
		for (const std::string& track_erase : tracks_erase)
		{
			m_available_kts.second.erase(track_erase);
		}

		if (tournamentHasIcy(m_tournament_game))
		{
			// ---------------
			//selectSoccerField("icy_soccer_field");
			if (m_available_kts.second.count("icy_soccer_field"))
			{
				m_available_kts.second.clear();
				m_available_kts.second.insert("icy_soccer_field");
			}
			else
			{
				m_available_kts.second.clear();
			}
			// ---------------
		}
		else if (tournamentHasTournamentField(m_tournament_game))
		{
			// ---------------
			//selectSoccerField("addon_tournament-field");
			if (m_available_kts.second.count("addon_supertournament-field"))
			{
				m_available_kts.second.clear();
				m_available_kts.second.insert("addon_supertournament-field");
			}
			else
			{
				m_available_kts.second.clear();
			}
			// ---------------
		}
		else if (tournamentHasGrass(m_tournament_game))
                {
                        // ---------------
                        //selectSoccerField("addon_tournament-field");
                        if (m_available_kts.second.count("addon_tournament-field") && m_available_kts.second.count("soccer_field") && m_available_kts.second.count("lasdunassoccer"))
                        {
                                m_available_kts.second.clear();
                                m_available_kts.second.insert("addon_tournament-field");
                                m_available_kts.second.insert("soccer_field");
                                m_available_kts.second.insert("lasdunassoccer");
                        }
                        else
                        {
                                m_available_kts.second.clear();
                        }
                        // ---------------
                }
	}
	
	m_command_voters.clear();

	if (m_gnu_elimination)
	{
		m_available_kts.first.insert(m_gnu_kart);
	}
	else
	{
		m_available_kts.first.insert("addon_alternative-tux");
		m_available_kts.first.insert("addon_android");
		m_available_kts.first.insert("addon_beagle_2");
		m_available_kts.first.insert("addon_blinky");
		m_available_kts.first.insert("addon_buggie");
		m_available_kts.first.insert("addon_dashie--cyber-bunny-");
		m_available_kts.first.insert("addon_elephpant");
		m_available_kts.first.insert("addon_evil-tux-v1");
		m_available_kts.first.insert("addon_fantasma-gnu");
		m_available_kts.first.insert("addon_firefox");
		m_available_kts.first.insert("addon_geeko");
		m_available_kts.first.insert("addon_hk-pig");
		m_available_kts.first.insert("addon_minix");
		m_available_kts.first.insert("addon_mozilla");
		m_available_kts.first.insert("addon_mr-iceblock");
		m_available_kts.first.insert("addon_penny_1");
		m_available_kts.first.insert("addon_python");
		m_available_kts.first.insert("addon_supertuxcart");
		m_available_kts.first.insert("addon_ubuntu");
		m_available_kts.first.insert("addon_vlc");
	}
	
    if (!m_tracks_queue.empty())
    {
        m_available_kts.second.clear();
        m_available_kts.second.insert(m_tracks_queue.front());
    }


    unsigned max_player = 0;
    STKHost::get()->updatePlayers(&max_player);
    if (auto ai = m_ai_peer.lock())
    {
        if (supportsAI())
        {
            unsigned total_ai_available =
                (unsigned)ai->getPlayerProfiles().size();
            m_ai_count = max_player > total_ai_available ?
                0 : total_ai_available - max_player + 1;
            // Disable ai peer for this game
            if (m_ai_count == 0)
                ai->setValidated(false);
            else
                ai->setValidated(true);
        }
        else
        {
            ai->setValidated(false);
            m_ai_count = 0;
        }
    }
    else
        m_ai_count = 0;

    if (RaceManager::get()->getMinorMode() == RaceManager::MINOR_MODE_FREE_FOR_ALL)
    {
        auto it = m_available_kts.second.begin();
        while (it != m_available_kts.second.end())
        {
            Track* t = track_manager->getTrack(*it);
            if (t->getMaxArenaPlayers() < max_player)
            {
                it = m_available_kts.second.erase(it);
            }
            else
                it++;
        }
    }
    
    m_global_filter.apply(max_player, m_available_kts.second);
    if (ServerConfig::m_soccer_tournament)
    {
        m_tournament_track_filters[m_tournament_game].apply(
            max_player, m_available_kts.second, m_tournament_arenas);
    }
   /* auto iter = m_available_kts.second.begin();
    while (iter != m_available_kts.second.end())
    {
        // Initial version which will be brought into a separate fuction
        std::string track = *iter;
        if (getTrackMaxPlayers(track) < max_player)
            iter = m_available_kts.second.erase(iter);
        else
            iter++;
    }*/

    if (m_available_kts.second.empty())
    {
        Log::error("ServerLobby", "No tracks for playing!");

		if (m_game_mode == 6) // soccer
			m_available_kts.second.insert("icy_soccer_field");
		else if (m_game_mode == 7 || m_game_mode == 8) // free for all, capture the flag
			m_available_kts.second.insert("stadium");
		else
			m_available_kts.second.insert("volcano_island"); // race

        // return;
    }

    RandomGenerator rg;
    std::set<std::string>::iterator it = m_available_kts.second.begin();
    std::advance(it, rg.get((int)m_available_kts.second.size()));
    m_default_vote->m_track_name = *it;
    switch (RaceManager::get()->getMinorMode())
    {
        case RaceManager::MINOR_MODE_NORMAL_RACE:
        case RaceManager::MINOR_MODE_TIME_TRIAL:
        case RaceManager::MINOR_MODE_FOLLOW_LEADER:
        {
            Track* t = track_manager->getTrack(*it);
            assert(t);
            m_default_vote->m_num_laps = t->getDefaultNumberOfLaps();
            m_default_vote->m_reverse = rg.get(2) == 0;
            break;
        }
        case RaceManager::MINOR_MODE_FREE_FOR_ALL:
        {
            m_default_vote->m_num_laps = 0;
            m_default_vote->m_reverse = rg.get(2) == 0;
            break;
        }
        case RaceManager::MINOR_MODE_CAPTURE_THE_FLAG:
        {
            m_default_vote->m_num_laps = 0;
            m_default_vote->m_reverse = 0;
            break;
        }
        case RaceManager::MINOR_MODE_SOCCER:
        {
            if (ServerConfig::m_soccer_tournament)
            {
                m_default_vote->m_num_laps = 10;
                m_default_vote->m_reverse = false;
            }
            else
            {
                if (m_game_setup->isSoccerGoalTarget())
                {
                    m_default_vote->m_num_laps =
                        (uint8_t)(UserConfigParams::m_num_goals);
                    if (m_default_vote->m_num_laps > 10)
                        m_default_vote->m_num_laps = (uint8_t)5;
                }
                else
                {
                    m_default_vote->m_num_laps =
                        (uint8_t)(UserConfigParams::m_soccer_time_limit);
                    if (m_default_vote->m_num_laps > 15)
                        m_default_vote->m_num_laps = (uint8_t)7;
                }
                m_default_vote->m_reverse = rg.get(2) == 0;
            }
            break;
        }
        default:
            assert(false);
            break;
    }

    if (!allowJoinedPlayersWaiting())
    {
        ProtocolManager::lock()->findAndTerminate(PROTOCOL_CONNECTION);
        if (m_server_id_online.load() != 0)
        {
            unregisterServer(false/*now*/,
                std::dynamic_pointer_cast<ServerLobby>(shared_from_this()));
        }
    }

    startVotingPeriod(ServerConfig::m_voting_timeout);
    NetworkString *ns = getNetworkString(1);
    // Start selection - must be synchronous since the receiver pushes
    // a new screen, which must be done from the main thread.
    ns->setSynchronous(true);
    ns->addUInt8(LE_START_SELECTION)
       .addFloat(ServerConfig::m_voting_timeout)
       .addUInt8(/*m_game_setup->isGrandPrixStarted() ? 1 : */0)
       .addUInt8((ServerConfig::m_fixed_lap_count >= 0
            || ServerConfig::m_auto_game_time_ratio > 0.0f) ? 1 : 0)
       .addUInt8(ServerConfig::m_track_voting ? 1 : 0);

    const auto& all_k = m_available_kts.first;
    const auto& all_t = m_available_kts.second;
    ns->addUInt16((uint16_t)all_k.size()).addUInt16((uint16_t)all_t.size());
    for (const std::string& kart : all_k)
    {
        ns->encodeString(kart);
    }
    for (const std::string& track : all_t)
    {
        ns->encodeString(track);
    }

    if (m_gnu_elimination && m_gnu_remained >= 0)
    {
        auto remaining_begin = m_gnu_participants.begin();
        auto remaining_end = remaining_begin + m_gnu_remained;
        STKHost::get()->sendPacketToAllPeersWith(
            [remaining_begin, remaining_end](STKPeer* p)
            {
                for (auto& profile : p->getPlayerProfiles())
                {
                    if (std::find(
                        remaining_begin, remaining_end,
                        StringUtils::wideToUtf8(profile->getName())) != remaining_end)
                    {
                        return true;
                    }
                }
                return false;
            }, ns, /*reliable*/true);
        delete ns;
        bool has_gnu = false;
        for (auto it = all_k.begin(); it != all_k.end(); it++)
        {
            has_gnu |= (*it == m_gnu_kart);
        }

        // The same NetworkString but without any non-Gnu karts
        NetworkString *ns = getNetworkString(1);
        ns->setSynchronous(true);
        ns->addUInt8(LE_START_SELECTION)
           .addFloat(ServerConfig::m_voting_timeout)
           .addUInt8(/*m_game_setup->isGrandPrixStarted() ? 1 : */0)
           .addUInt8((ServerConfig::m_fixed_lap_count >= 0
            || ServerConfig::m_auto_game_time_ratio > 0.0f) ? 1 : 0)
           .addUInt8(ServerConfig::m_track_voting ? 1 : 0);

        ns->addUInt16((uint16_t)(has_gnu ? 1 : 0)).addUInt16(
            (uint16_t)all_t.size());
        if (has_gnu)
        {
            ns->encodeString(std::string(m_gnu_kart));
        }
        for (const std::string& track : all_t)
        {
            ns->encodeString(track);
        }

        STKHost::get()->sendPacketToAllPeersWith(
            [remaining_begin, remaining_end](STKPeer* p)
            {
                for (auto& profile : p->getPlayerProfiles())
                {
                    if (std::find(
                        remaining_begin, remaining_end,
                        StringUtils::wideToUtf8(profile->getName()))
                        != remaining_end)
                    {
                        return false;
                    }
                }
                return true;
            }, ns, /*reliable*/true);
    }
    else
    {
        // std::set<std::string> all_players;
        // for (const std::string& s: m_tournament_red_players) {
        //     all_players.insert(s);
        // }   
        // for (const std::string& s: m_tournament_blue_players) {
        //     all_players.insert(s);
        // }
        STKHost::get()->sendPacketToAllPeersWith(
            [/*all_players*/this](STKPeer* p) -> bool
            {
				std::string username = StringUtils::wideToUtf8(
					p->getPlayerProfiles()[0]->getName());
				// return all_players.count(username) > 0;
				bool hasKartFreedom = m_set_kart.count(username) == 0;
				return canRace(p) && hasKartFreedom;
            }, ns, /*reliable*/true);
        delete ns;

		// After setkart command only one kart is available
		for (auto& username_kart : m_set_kart)
		{
			NetworkString *ns_fixedKart = getNetworkString(1);
			ns_fixedKart->setSynchronous(true);
			ns_fixedKart->addUInt8(LE_START_SELECTION)
				.addFloat(ServerConfig::m_voting_timeout)
				.addUInt8(/*m_game_setup->isGrandPrixStarted() ? 1 : */0)
				.addUInt8((ServerConfig::m_fixed_lap_count >= 0
					|| ServerConfig::m_auto_game_time_ratio > 0.0f) ? 1 : 0)
				.addUInt8(ServerConfig::m_track_voting ? 1 : 0);

			ns_fixedKart->addUInt16(1).addUInt16((uint16_t)all_t.size());

			ns_fixedKart->encodeString(username_kart.second);

			for (const std::string& track : all_t)
			{
				ns_fixedKart->encodeString(track);
			}

			STKHost::get()->sendPacketToAllPeersWith(
				[this, username_kart](STKPeer* p) -> bool
			{
				std::string username = StringUtils::wideToUtf8(
					p->getPlayerProfiles()[0]->getName());

				return canRace(p) && username == username_kart.first;
			}, ns_fixedKart, /*reliable*/true);

			delete ns_fixedKart;
		}
    }

    m_state = SELECTING;
    if (need_to_update || !always_spectate_peers.empty())
    {
        NetworkString* back_lobby = getNetworkString(2);
        back_lobby->setSynchronous(true);
        back_lobby->addUInt8(LE_BACK_LOBBY).addUInt8(BLR_SPECTATING_NEXT_GAME);
        STKHost::get()->sendPacketToAllPeersWith(
            [always_spectate_peers](STKPeer* peer) {
            return always_spectate_peers.find(peer) !=
            always_spectate_peers.end(); }, back_lobby, /*reliable*/true);
        delete back_lobby;
        updatePlayerList();
    }

    if (!allowJoinedPlayersWaiting())
    {
        // Drop all pending players and keys if doesn't allow joinning-waiting
        for (auto& p : m_pending_connection)
        {
            if (auto peer = p.first.lock())
                peer->disconnect();
        }
        m_pending_connection.clear();
        std::unique_lock<std::mutex> ul(m_keys_mutex);
        m_keys.clear();
        ul.unlock();
    }

    // Will be changed after the first vote received
    m_timeout.store(std::numeric_limits<int64_t>::max());
    if (!m_game_setup->isGrandPrixStarted())
        m_gp_scores.clear();
}   // startSelection

//-----------------------------------------------------------------------------
/** Query the STK server for connection requests. For each connection request
 *  start a ConnectToPeer protocol.
 */
void ServerLobby::checkIncomingConnectionRequests()
{
    // First poll every 5 seconds. Return if no polling needs to be done.
    const uint64_t POLL_INTERVAL = 5000;
    static uint64_t last_poll_time = 0;
    if (StkTime::getMonoTimeMs() < last_poll_time + POLL_INTERVAL ||
        StkTime::getMonoTimeMs() > m_last_success_poll_time.load() + 30000)
        return;

    // Keep the port open, it can be sent to anywhere as we will send to the
    // correct peer later in ConnectToPeer.
    if (ServerConfig::m_firewalled_server)
    {
        BareNetworkString data;
        data.addUInt8(0);
        const SocketAddress* stun_v4 = STKHost::get()->getStunIPv4Address();
        const SocketAddress* stun_v6 = STKHost::get()->getStunIPv6Address();
        if (stun_v4)
            STKHost::get()->sendRawPacket(data, *stun_v4);
        if (stun_v6)
            STKHost::get()->sendRawPacket(data, *stun_v6);
    }

    // Now poll the stk server
    last_poll_time = StkTime::getMonoTimeMs();

    // ========================================================================
    class PollServerRequest : public Online::XMLRequest
    {
    private:
        std::weak_ptr<ServerLobby> m_server_lobby;
        std::weak_ptr<ProtocolManager> m_protocol_manager;
    protected:
        virtual void afterOperation()
        {
            Online::XMLRequest::afterOperation();
            const XMLNode* result = getXMLData();
            std::string success;

            if (!result->get("success", &success) || success != "yes")
            {
                Log::error("ServerLobby", "Poll server request failed: %s",
                    StringUtils::wideToUtf8(getInfo()).c_str());
                return;
            }

            // Now start a ConnectToPeer protocol for each connection request
            const XMLNode * users_xml = result->getNode("users");
            std::map<uint32_t, KeyData> keys;
            auto sl = m_server_lobby.lock();
            if (!sl)
                return;
            sl->m_last_success_poll_time.store(StkTime::getMonoTimeMs());
            if (sl->m_state.load() != WAITING_FOR_START_GAME &&
                !sl->allowJoinedPlayersWaiting())
            {
                sl->replaceKeys(keys);
                return;
            }

            sl->removeExpiredPeerConnection();
            for (unsigned int i = 0; i < users_xml->getNumNodes(); i++)
            {
                uint32_t addr, id;
                uint16_t port;
                std::string ipv6;
                users_xml->getNode(i)->get("ip", &addr);
                users_xml->getNode(i)->get("ipv6", &ipv6);
                users_xml->getNode(i)->get("port", &port);
                users_xml->getNode(i)->get("id", &id);
                users_xml->getNode(i)->get("aes-key", &keys[id].m_aes_key);
                users_xml->getNode(i)->get("aes-iv", &keys[id].m_aes_iv);
                users_xml->getNode(i)->get("username", &keys[id].m_name);
                users_xml->getNode(i)->get("country-code",
                    &keys[id].m_country_code);
                keys[id].m_tried = false;
                if (ServerConfig::m_firewalled_server)
                {
                    SocketAddress peer_addr(addr, port);
                    if (!ipv6.empty())
                        peer_addr.init(ipv6, port);
                    peer_addr.convertForIPv6Socket(isIPv6Socket());
                    std::string peer_addr_str = peer_addr.toString();
                    if (sl->m_pending_peer_connection.find(peer_addr_str) !=
                        sl->m_pending_peer_connection.end())
                    {
                        continue;
                    }
                    auto ctp = std::make_shared<ConnectToPeer>(peer_addr);
                    if (auto pm = m_protocol_manager.lock())
                        pm->requestStart(ctp);
                    sl->addPeerConnection(peer_addr_str);
                }
            }
            sl->replaceKeys(keys);
        }
    public:
        PollServerRequest(std::shared_ptr<ServerLobby> sl,
                          std::shared_ptr<ProtocolManager> pm)
        : XMLRequest(Online::RequestManager::HTTP_MAX_PRIORITY),
        m_server_lobby(sl), m_protocol_manager(pm)
        {
            m_disable_sending_log = true;
        }
    };   // PollServerRequest
    // ========================================================================

    auto request = std::make_shared<PollServerRequest>(
        std::dynamic_pointer_cast<ServerLobby>(shared_from_this()),
        ProtocolManager::lock());
    NetworkConfig::get()->setServerDetails(request,
        "poll-connection-requests");
    const SocketAddress& addr = STKHost::get()->getPublicAddress();
    request->addParameter("address", addr.getIP()  );
    request->addParameter("port",    addr.getPort());
    request->addParameter("current-players", getLobbyPlayers());
    request->addParameter("game-started",
        m_state.load() == WAITING_FOR_START_GAME ? 0 : 1);
    std::string current_track = getPlayingTrackIdent();
    if (!current_track.empty())
        request->addParameter("current-track", getPlayingTrackIdent());
    request->queue();

}   // checkIncomingConnectionRequests

//-----------------------------------------------------------------------------
/** Checks if the race is finished, and if so informs the clients and switches
 *  to state RESULT_DISPLAY, during which the race result gui is shown and all
 *  clients can click on 'continue'.
 */
void ServerLobby::checkRaceFinished()
{
    assert(RaceEventManager::get()->isRunning());
    assert(World::getWorld());
    if (!RaceEventManager::get()->isRaceOver()) return;

    if (ServerConfig::m_soccer_tournament)
    {
        World* w = World::getWorld();
        if (w)
        {
            SoccerWorld *sw = dynamic_cast<SoccerWorld*>(w);
            sw->tellCountIfDiffers();
        }
    }
    Log::info("ServerLobby", "The game is considered finished.");
    // notify the network world that it is stopped
    RaceEventManager::get()->stop();

    // stop race protocols before going back to lobby (end race)
    RaceEventManager::get()->getProtocol()->requestTerminate();
    GameProtocol::lock()->requestTerminate();

    // Save race result before delete the world
    m_result_ns->clear();
    m_result_ns->addUInt8(LE_RACE_FINISHED);
    if (m_game_setup->isGrandPrix())
    {
        // fastest lap
        int fastest_lap =
            static_cast<LinearWorld*>(World::getWorld())->getFastestLapTicks();
        m_result_ns->addUInt32(fastest_lap);
        irr::core::stringw fastest_kart_wide =
            static_cast<LinearWorld*>(World::getWorld())
            ->getFastestLapKartName();
        m_result_ns->encodeString(fastest_kart_wide);
        std::string fastest_kart = StringUtils::wideToUtf8(fastest_kart_wide);

        int points_fl = 0;
        int points_pole = 0;
        WorldWithRank *wwr = dynamic_cast<WorldWithRank*>(World::getWorld());
        if (wwr)
        {
            points_fl = wwr->getFastestLapPoints();
            points_pole = wwr->getPolePoints();
        }
        else
        {
            Log::error("ServerLobby",
                       "World with scores that is not a WorldWithRank??");
        }

        // all gp tracks
        m_result_ns->addUInt8((uint8_t)m_game_setup->getTotalGrandPrixTracks())
            .addUInt8((uint8_t)m_game_setup->getAllTracks().size());
        for (const std::string& gp_track : m_game_setup->getAllTracks())
            m_result_ns->encodeString(gp_track);

        // each kart score and total time
        m_result_ns->addUInt8((uint8_t)RaceManager::get()->getNumPlayers());
        for (unsigned i = 0; i < RaceManager::get()->getNumPlayers(); i++)
        {
            int last_score = RaceManager::get()->getKartScore(i);
            int cur_score = last_score;
            float overall_time = RaceManager::get()->getOverallTime(i);
            if (auto player =
                RaceManager::get()->getKartInfo(i).getNetworkPlayerProfile().lock())
            {
                std::string username = StringUtils::wideToUtf8(player->getName());
                last_score = m_gp_scores[username].score;
                cur_score += last_score;
                if (username == fastest_kart)
                    cur_score += points_fl;

                overall_time = overall_time + m_gp_scores[username].time;
                player->setScore(cur_score);
                player->setOverallTime(overall_time);

                m_gp_scores[username].score = cur_score;
                m_gp_scores[username].time = overall_time;
            }
            m_result_ns->addUInt32(last_score).addUInt32(cur_score)
                .addFloat(overall_time);            
        }
    }
    else if (RaceManager::get()->modeHasLaps())
    {
        int fastest_lap =
            static_cast<LinearWorld*>(World::getWorld())->getFastestLapTicks();
        m_result_ns->addUInt32(fastest_lap);
        m_result_ns->encodeString(static_cast<LinearWorld*>(World::getWorld())
            ->getFastestLapKartName());
    }

    uint8_t ranking_changes_indication = 0;
    if (ServerConfig::m_ranked && RaceManager::get()->modeHasLaps())
        ranking_changes_indication = 1;
    m_result_ns->addUInt8(ranking_changes_indication);

    if (m_gnu_elimination) {
        updateGnuElimination();
    }

    if (ServerConfig::m_store_results)
    {
        bool racing_mode = false;
        racing_mode |= RaceManager::get()->getMinorMode() ==
            RaceManager::MINOR_MODE_NORMAL_RACE;
        racing_mode |= RaceManager::get()->getMinorMode() ==
            RaceManager::MINOR_MODE_TIME_TRIAL;
        if (racing_mode)
            storeResults();
    }

    if (ServerConfig::m_ranked)
    {
        computeNewRankings();
        submitRankingsToAddons();
    }
    m_state.store(WAIT_FOR_RACE_STOPPED);

    if (!m_tracks_queue.empty())
    {
        m_tracks_queue.pop_front();
        // Reload GP tracks if GP ends
        if (m_tracks_queue.empty() && m_game_setup->isGrandPrix())
            loadTracksQueueFromConfig();
    }
}   // checkRaceFinished

//-----------------------------------------------------------------------------
/** Compute the new player's rankings used in ranked servers
 */
void ServerLobby::computeNewRankings()
{
    // No ranking for battle mode
    if (!RaceManager::get()->modeHasLaps())
        return;

    // Using a vector of vector, it would be possible to fill
    // all j < i v[i][j] with -v[j][i]
    // Would this be worth it ?
    std::vector<double> scores_change;
    std::vector<double> new_scores;
    std::vector<double> prev_scores;

    unsigned player_count = RaceManager::get()->getNumPlayers();
    m_result_ns->addUInt8((uint8_t)player_count);
    for (unsigned i = 0; i < player_count; i++)
    {
        const uint32_t id = RaceManager::get()->getKartInfo(i).getOnlineId();
        double prev_score = m_scores.at(id);
        new_scores.push_back(prev_score);
        new_scores[i] += distributeBasePoints(id);
        prev_scores.push_back(prev_score);
    }
 
    // First, update the number of ranked races
    for (unsigned i = 0; i < player_count; i++)
    {
         const uint32_t id = RaceManager::get()->getKartInfo(i).getOnlineId();
         m_num_ranked_races.at(id)++;
    }

    // Now compute points exchanges
    for (unsigned i = 0; i < player_count; i++)
    {
        scores_change.push_back(0.0);

        World* w = World::getWorld();
        assert(w);
        double player1_scores = new_scores[i];
        // If the player has quitted before the race end,
        // the value will be incorrect, but it will not be used
        double player1_time  = RaceManager::get()->getKartRaceTime(i);
        double player1_factor =
            computeRankingFactor(RaceManager::get()->getKartInfo(i).getOnlineId());
        double player1_handicap = (   w->getKart(i)->getHandicap()
                                   == HANDICAP_NONE               ) ? 0 : HANDICAP_OFFSET;

        for (unsigned j = 0; j < player_count; j++)
        {
            // Don't compare a player with himself
            if (i == j)
                continue;

            double result = 0.0;
            double expected_result = 0.0;
            double ranking_importance = 0.0;
            double max_time = 0.0;

            // No change between two quitting players
            if (w->getKart(i)->isEliminated() &&
                w->getKart(j)->isEliminated())
                continue;

            double player2_scores = new_scores[j];
            double player2_time = RaceManager::get()->getKartRaceTime(j);
            double player2_handicap = (   w->getKart(j)->getHandicap()
                                       == HANDICAP_NONE               ) ? 0 : HANDICAP_OFFSET;

            // Compute the result and race ranking importance
            double player_factors = std::min(player1_factor,
                computeRankingFactor(
                RaceManager::get()->getKartInfo(j).getOnlineId()));

            double mode_factor = getModeFactor();

            if (w->getKart(i)->isEliminated())
            {
                result = 0.0;
                player1_time = player2_time; // for getTimeSpread
                max_time = MAX_SCALING_TIME;
            }
            else if (w->getKart(j)->isEliminated())
            {
                result = 1.0;
                player2_time = player1_time;
                max_time = MAX_SCALING_TIME;
            }
            else
            {
                // If time difference > 2,5% ; the result is 1 or 0
                // Otherwise, it is averaged between 0 and 1.
                if (player1_time <= player2_time)
                {
                    result =
                        (player2_time - player1_time) / (player1_time / 20.0);
                    result = std::min(1.0, 0.5 + result);
                }
                else
                {
                    result =
                        (player1_time - player2_time) / (player2_time / 20.0);
                    result = std::max(0.0, 0.5 - result);
                }

                max_time = std::min(std::max(player1_time, player2_time),
                    MAX_SCALING_TIME);
            }

            ranking_importance = mode_factor *
                scalingValueForTime(max_time) * player_factors;

            // Compute the expected result using an ELO-like function
            double diff = player2_scores - player1_scores;

            if (!w->getKart(i)->isEliminated() && !w->getKart(j)->isEliminated())
                diff += player1_handicap - player2_handicap;

            double uncertainty = std::max(getUncertaintySpread(RaceManager::get()->getKartInfo(i).getOnlineId()),
                                          getUncertaintySpread(RaceManager::get()->getKartInfo(j).getOnlineId()) );

            expected_result = 1.0/ (1.0 + std::pow(10.0,
                diff / (  BASE_RANKING_POINTS / 2.0
                        * getModeSpread()
                        * getTimeSpread(std::min(player1_time, player2_time))
                        * uncertainty )));

            // Compute the ranking change
            scores_change[i] +=
                ranking_importance * (result - expected_result);
        }
    }

    // Don't merge it in the main loop as new_scores value are used there
    for (unsigned i = 0; i < player_count; i++)
    {
        new_scores[i] += scores_change[i];
        const uint32_t id = RaceManager::get()->getKartInfo(i).getOnlineId();
        m_scores.at(id) =  new_scores[i];
        if (m_scores.at(id) > m_max_scores.at(id))
            m_max_scores.at(id) = m_scores.at(id);
    }

    for (unsigned i = 0; i < player_count; i++)
    {
        const uint32_t id = RaceManager::get()->getKartInfo(i).getOnlineId();
        double change = m_scores.at(id) - prev_scores[i];
        m_result_ns->addFloat((float)change);
    }
}   // computeNewRankings

//-----------------------------------------------------------------------------
/** Compute the ranking factor, used to make top rankings more stable
 *  and to allow new players to faster get to an appropriate ranking
 */
double ServerLobby::computeRankingFactor(uint32_t online_id)
{
    double max_points = m_max_scores.at(online_id);
    unsigned num_races = m_num_ranked_races.at(online_id);

    if (max_points >= (BASE_RANKING_POINTS * 2.0))
        return 0.6;
    else if (max_points >= (BASE_RANKING_POINTS * 1.75) || num_races > 500)
        return 0.7;
    else if (max_points >= (BASE_RANKING_POINTS * 1.5) || num_races > 250)
        return 0.8;
    else if (max_points >= (BASE_RANKING_POINTS * 1.25) || num_races > 100)
        return 1.0;
    // The base ranking points are not distributed all at once
    // So it's not guaranteed a player reach them
    else if (max_points >= (BASE_RANKING_POINTS) || num_races > 50)
        return 1.2;
    else
        return 1.5;

}   // computeRankingFactor

//-----------------------------------------------------------------------------
/** Returns the mode race importance factor,
 *  used to make ranking move slower in more random modes.
 */
double ServerLobby::getModeFactor()
{
    if (RaceManager::get()->isTimeTrialMode())
        return 1.0;
    return 0.7;
}   // getModeFactor

//-----------------------------------------------------------------------------
/** Returns the mode spread factor, used so that a similar difference in
 *  skill will result in a similar ranking difference in more random modes.
 */
double ServerLobby::getModeSpread()
{
    if (RaceManager::get()->isTimeTrialMode())
        return 1.0;

    //TODO: the value used here for normal races is a wild guess.
    // When hard data to the spread tendencies of time-trial
    // and normal mode becomes available, update this to make
    // the spreads more comparable
    return 1.5;
}   // getModeSpread

//-----------------------------------------------------------------------------
/** Returns the time spread factor.
 *  Short races are more random, so the expected result changes depending
 *  on race duration.
 */
double ServerLobby::getTimeSpread(double time)
{
    return sqrt(120.0 / time);
}   // getTimeSpread

//-----------------------------------------------------------------------------
/** Returns the uncertainty spread factor.
 *  The ranking of new players is not yet reliable,
 *  so weight the expected results twoards 0.5 by using a > 1 spread
 */
double ServerLobby::getUncertaintySpread(uint32_t online_id)
{
    unsigned num_races  = m_num_ranked_races.at(online_id);
    if (num_races <= 60)
        return 0.5 + (4.0/sqrt(num_races+3));
    else
        return 1.0;
}   // getUncertaintySpread

//-----------------------------------------------------------------------------
/** Compute the scaling value of a given time
 *  This is linear to race duration, getTimeSpread takes care
 *  of expecting a more random result in shorter races.
 */
double ServerLobby::scalingValueForTime(double time)
{
    return time * MAX_POINTS_PER_SECOND;
}   // scalingValueForTime

//-----------------------------------------------------------------------------
/** Manages the distribution of the base points.
 *  Gives half of the points progressively
 *  by smaller and smaller chuncks from race 1 to 60.
 *  The race count is incremented after this is called, so num_races
 *  is between 0 and 59.
 *  The first half is distributed when the player enters
 *  for the first time in a ranked server.
 */
double ServerLobby::distributeBasePoints(uint32_t online_id)
{
    unsigned num_races  = m_num_ranked_races.at(online_id);
    if (num_races < 60)
    {
        return BASE_RANKING_POINTS / 8000.0 * std::max((96u - num_races), 41u);
    }
    else
        return 0.0;
}   // distributeBasePoints

//-----------------------------------------------------------------------------
/** Called when a client disconnects.
 *  \param event The disconnect event.
 */
void ServerLobby::clientDisconnected(Event* event)
{
    auto players_on_peer = event->getPeer()->getPlayerProfiles();
    if (players_on_peer.empty())
        return;

    NetworkString* msg = getNetworkString(2);
    const bool waiting_peer_disconnected =
        event->getPeer()->isWaitingForGame();
    msg->setSynchronous(true);
    msg->addUInt8(LE_PLAYER_DISCONNECTED);
    msg->addUInt8((uint8_t)players_on_peer.size())
        .addUInt32(event->getPeer()->getHostId());

	if (m_player_queue_limit > 0)
	{
		auto peer_sp = event->getPeerSP();
        addDeletePlayersFromQueue(peer_sp, false);
	}
		
    for (auto p : players_on_peer)
    {
        std::string name = StringUtils::wideToUtf8(p->getName());
        msg->encodeString(name);
        Log::info("ServerLobby", "%s disconnected", name.c_str());
        
        if (RaceEventManager::get())
        {
            if (ServerConfig::m_save_goals && RaceEventManager::get()->isRunning())
            {
                double phase = 0.0;
                if (RaceManager::get()->hasTimeTarget())
                {
                    phase = (RaceManager::get()->getTimeTarget() - World::getWorld()->getTime())/RaceManager::get()->getTimeTarget();
                }
                else
                {
                    int red_scorers_count = 0; int blue_scorers_count = 0;
                    SoccerWorld *sw = dynamic_cast<SoccerWorld*>(World::getWorld());
                    if (sw)
                    {
                        red_scorers_count = sw->get_red_scorers_count();
                        blue_scorers_count = sw->get_blue_scorers_count();
                    }
                    phase = 1.0*std::max(red_scorers_count, blue_scorers_count)/RaceManager::get()->getMaxGoal();
                    std::string message = "red_scorers_cnt=" + std::to_string(red_scorers_count) + " / blue_scorers_cnt" + std::to_string(blue_scorers_count);
                    message += " / max_goll=" + std::to_string(RaceManager::get()->getMaxGoal());
                    Log::info("ServerLobby", message.c_str());
                }
                std::string message = "phase=" + std::to_string(phase);
                Log::info("ServerLobby", message.c_str());
                rem_gamescore3(name,phase);
            }
        }
    }
    
	// This prevents the server from crashing - please do not remove!
	if (m_peers_ready.find(event->getPeerSP()) != m_peers_ready.end())
		m_peers_ready.erase(event->getPeerSP());

    unsigned players_number;
    STKHost::get()->updatePlayers(NULL, NULL, &players_number);
    if (players_number == 0)
        resetToDefaultSettings();

    // Don't show waiting peer disconnect message to in game player
    STKHost::get()->sendPacketToAllPeersWith([waiting_peer_disconnected]
        (STKPeer* p)
        {
            if (!p->isValidated())
                return false;
            if (!p->isWaitingForGame() && waiting_peer_disconnected)
                return false;
            return true;
        }, msg);
    updatePlayerList();
    delete msg;

    writeDisconnectInfoTable(event->getPeer());
}   // clientDisconnected

//-----------------------------------------------------------------------------
void ServerLobby::clearDisconnectedRankedPlayer()
{
    for (auto it = m_ranked_players.begin(); it != m_ranked_players.end();)
    {
        if (it->second.expired())
        {
            const uint32_t id = it->first;
            m_scores.erase(id);
            m_max_scores.erase(id);
            m_num_ranked_races.erase(id);
            it = m_ranked_players.erase(it);
        }
        else
        {
            it++;
        }
    }
}   // clearDisconnectedRankedPlayer

//-----------------------------------------------------------------------------
void ServerLobby::kickPlayerWithReason(STKPeer* peer, const char* reason) const
{
    NetworkString *message = getNetworkString(2);
    message->setSynchronous(true);
    message->addUInt8(LE_CONNECTION_REFUSED).addUInt8(RR_BANNED);
    message->encodeString(std::string(reason));
    peer->cleanPlayerProfiles();
    peer->sendPacket(message, true/*reliable*/, false/*encrypted*/);
    peer->reset();
    delete message;
}   // kickPlayerWithReason

//-----------------------------------------------------------------------------
void ServerLobby::saveIPBanTable(const SocketAddress& addr)
{
#ifdef ENABLE_SQLITE3
    if (addr.isIPv6() || !m_db || !m_ip_ban_table_exists)
        return;

    std::string query = StringUtils::insertValues(
        "INSERT INTO %s (ip_start, ip_end) "
        "VALUES (%u, %u);",
        ServerConfig::m_ip_ban_table.c_str(), addr.getIP(), addr.getIP());
    easySQLQuery(query);
#endif
}   // saveIPBanTable

//-----------------------------------------------------------------------------
bool ServerLobby::handleAssets(const NetworkString& ns, STKPeer* peer)
{
    std::set<std::string> client_karts, client_tracks;
    const unsigned kart_num = ns.getUInt16();
    const unsigned track_num = ns.getUInt16();
    for (unsigned i = 0; i < kart_num; i++)
    {
        std::string kart;
        ns.decodeString(&kart);
        client_karts.insert(kart);
    }
    for (unsigned i = 0; i < track_num; i++)
    {
        std::string track;
        ns.decodeString(&track);
        client_tracks.insert(track);
    }

    // Drop this player if he doesn't have at least 1 kart / track the same
    // as server
    float okt = 0.0f;
    float ott = 0.0f;
    int addon_karts = 0;
    int addon_tracks = 0;
    int addon_arenas = 0;
    int addon_soccers = 0;
    for (auto& client_kart : client_karts)
    {
        if (m_official_kts.first.find(client_kart) !=
            m_official_kts.first.end())
            okt += 1.0f;
        if (m_addon_kts.first.find(client_kart) !=
            m_addon_kts.first.end())
            ++addon_karts;
    }
    okt = okt / (float)m_official_kts.first.size();
    for (auto& client_track : client_tracks)
    {
        if (m_official_kts.second.find(client_track) !=
            m_official_kts.second.end())
            ott += 1.0f;
        if (m_addon_kts.second.find(client_track) !=
            m_addon_kts.second.end())
            ++addon_tracks;
        if (m_addon_arenas.find(client_track) !=
            m_addon_arenas.end())
            ++addon_arenas;
        if (m_addon_soccers.find(client_track) !=
            m_addon_soccers.end())
            ++addon_soccers;
    }
    ott = ott / (float)m_official_kts.second.size();

    std::set<std::string> karts_erase, tracks_erase;
    for (const std::string& server_kart : m_entering_kts.first)
    {
        if (client_karts.find(server_kart) == client_karts.end())
        {
            karts_erase.insert(server_kart);
        }
    }
    for (const std::string& server_track : m_entering_kts.second)
    {
        if (client_tracks.find(server_track) == client_tracks.end())
        {
            tracks_erase.insert(server_track);
        }
    }

    bool has_required_tracks = true;
    for (const std::string& required_track : m_must_have_tracks)
    {
        if (client_tracks.find(required_track) == client_tracks.end())
        {
            has_required_tracks = false;
            Log::info("ServerLobby", "Player does not have a required track '%s'.", required_track.c_str());
            break;
        }
    }

    Log::info("ServerLobby", "Player has the following addons: %d/%d karts,"
        " %d/%d tracks, %d/%d arenas, %d/%d soccer fields.", addon_karts,
        (int)ServerConfig::m_addon_karts_threshold, addon_tracks,
        (int)ServerConfig::m_addon_tracks_threshold, addon_arenas,
        (int)ServerConfig::m_addon_arenas_threshold, addon_soccers,
        (int)ServerConfig::m_addon_soccers_threshold);

    peer->addon_karts_count = addon_karts;
    peer->addon_tracks_count = addon_tracks;
    peer->addon_arenas_count = addon_arenas;
    peer->addon_soccers_count = addon_soccers;

    if (karts_erase.size() == m_entering_kts.first.size())
        Log::verbose("ServerLobby", "Bad player: no common karts with server");
    if (tracks_erase.size() == m_entering_kts.second.size())
        Log::verbose("ServerLobby", "Bad player: no common tracks with server");
    if (okt < ServerConfig::m_official_karts_threshold)
        Log::verbose("ServerLobby", "Bad player: bad official kart threshold");
    if (ott < ServerConfig::m_official_tracks_threshold)
        Log::verbose("ServerLobby", "Bad player: bad official track threshold");
    if (addon_karts < (int)ServerConfig::m_addon_karts_threshold)
        Log::verbose("ServerLobby", "Bad player: too little addon karts");
    if (addon_tracks < (int)ServerConfig::m_addon_tracks_threshold)
        Log::verbose("ServerLobby", "Bad player: too little addon tracks");
    if (addon_arenas < (int)ServerConfig::m_addon_arenas_threshold)
        Log::verbose("ServerLobby", "Bad player: too little addon arenas");
    if (addon_soccers < (int)ServerConfig::m_addon_soccers_threshold)
        Log::verbose("ServerLobby", "Bad player: too little addon soccers");
    if (!has_required_tracks)
        Log::verbose("ServerLobby", "Bad player: no required tracks");

    if (karts_erase.size() == m_entering_kts.first.size() ||
        tracks_erase.size() == m_entering_kts.second.size() ||
        okt < ServerConfig::m_official_karts_threshold ||
        ott < ServerConfig::m_official_tracks_threshold ||
        addon_karts < (int)ServerConfig::m_addon_karts_threshold ||
        addon_tracks < (int)ServerConfig::m_addon_tracks_threshold ||
        addon_arenas < (int)ServerConfig::m_addon_arenas_threshold ||
        addon_soccers < (int)ServerConfig::m_addon_soccers_threshold ||
        !has_required_tracks)
    {
        NetworkString *message = getNetworkString(2);
        message->setSynchronous(true);
        message->addUInt8(LE_CONNECTION_REFUSED)
            .addUInt8(RR_INCOMPATIBLE_DATA);

        std::string advice = ServerConfig::m_incompatible_advice;
        if (!advice.empty())
        {
            NetworkString* incompatible_reason = getNetworkString();
            incompatible_reason->addUInt8(LE_CHAT);
            incompatible_reason->setSynchronous(true);
            incompatible_reason->encodeString16(
                StringUtils::utf8ToWide(advice));
            peer->sendPacket(incompatible_reason,
                true/*reliable*/, false/*encrypted*/);
            Log::info("ServerLobby", "Sent advice");
            delete incompatible_reason;
        }

        peer->cleanPlayerProfiles();
        peer->sendPacket(message, true/*reliable*/, false/*encrypted*/);
        peer->reset();
        delete message;
        Log::verbose("ServerLobby", "Player has incompatible karts / tracks.");
        return false;
    }

    std::array<int, AS_TOTAL> addons_scores = {{ -1, -1, -1, -1 }};
    size_t addon_kart = 0;
    size_t addon_track = 0;
    size_t addon_arena = 0;
    size_t addon_soccer = 0;

    for (auto& kart : m_addon_kts.first)
    {
        if (client_karts.find(kart) != client_karts.end())
            addon_kart++;
    }
    for (auto& track : m_addon_kts.second)
    {
        if (client_tracks.find(track) != client_tracks.end())
            addon_track++;
    }
    for (auto& arena : m_addon_arenas)
    {
        if (client_tracks.find(arena) != client_tracks.end())
            addon_arena++;
    }
    for (auto& soccer : m_addon_soccers)
    {
        if (client_tracks.find(soccer) != client_tracks.end())
            addon_soccer++;
    }

    if (!m_addon_kts.first.empty())
    {
        addons_scores[AS_KART] = int
            ((float)addon_kart / (float)m_addon_kts.first.size() * 100.0);
    }
    if (!m_addon_kts.second.empty())
    {
        addons_scores[AS_TRACK] = int
            ((float)addon_track / (float)m_addon_kts.second.size() * 100.0);
    }
    if (!m_addon_arenas.empty())
    {
        addons_scores[AS_ARENA] = int
            ((float)addon_arena / (float)m_addon_arenas.size() * 100.0);
    }
    if (!m_addon_soccers.empty())
    {
        addons_scores[AS_SOCCER] = int
            ((float)addon_soccer / (float)m_addon_soccers.size() * 100.0);
    }

    // Save available karts and tracks from clients in STKPeer so if this peer
    // disconnects later in lobby it won't affect current players
    peer->setAvailableKartsTracks(client_karts, client_tracks);
    peer->setAddonsScores(addons_scores);

    if (m_process_type == PT_CHILD &&
        peer->getHostId() == m_client_server_host_id.load())
    {
        // Update child process addons list too so player can choose later
        updateAddons();
        updateTracksForMode();
    }
    return true;
}   // handleAssets

//-----------------------------------------------------------------------------
void ServerLobby::connectionRequested(Event* event)
{
    std::shared_ptr<STKPeer> peer = event->getPeerSP();
    NetworkString& data = event->data();
    if (!checkDataSize(event, 14)) return;

    peer->cleanPlayerProfiles();

    // can we add the player ?
    if (!allowJoinedPlayersWaiting() &&
        (m_state.load() != WAITING_FOR_START_GAME /*||
        m_game_setup->isGrandPrixStarted()*/))
    {
        NetworkString *message = getNetworkString(2);
        message->setSynchronous(true);
        message->addUInt8(LE_CONNECTION_REFUSED).addUInt8(RR_BUSY);
        // send only to the peer that made the request and disconnect it now
        peer->sendPacket(message, true/*reliable*/, false/*encrypted*/);
        peer->reset();
        delete message;
        Log::verbose("ServerLobby", "Player refused: selection started");
        return;
    }

    // Check server version
    int version = data.getUInt32();
    if (version < stk_config->m_min_server_version ||
        version > stk_config->m_max_server_version)
    {
        NetworkString *message = getNetworkString(2);
        message->setSynchronous(true);
        message->addUInt8(LE_CONNECTION_REFUSED)
                .addUInt8(RR_INCOMPATIBLE_DATA);
        peer->sendPacket(message, true/*reliable*/, false/*encrypted*/);
        peer->reset();
        delete message;
        Log::verbose("ServerLobby", "Player refused: wrong server version");
        return;
    }
    std::string user_version;
    data.decodeString(&user_version);
    event->getPeer()->setUserVersion(user_version);

    unsigned list_caps = data.getUInt16();
    std::set<std::string> caps;
    for (unsigned i = 0; i < list_caps; i++)
    {
        std::string cap;
        data.decodeString(&cap);
        caps.insert(cap);
    }
    event->getPeer()->setClientCapabilities(caps);
    if (!handleAssets(data, event->getPeer()))
        return;

    unsigned player_count = data.getUInt8();
    uint32_t online_id = 0;
    uint32_t encrypted_size = 0;
    online_id = data.getUInt32();
    encrypted_size = data.getUInt32();

    // Will be disconnected if banned by IP
    testBannedForIP(peer.get());
    if (peer->isDisconnected())
        return;

    testBannedForIPv6(peer.get());
    if (peer->isDisconnected())
        return;

    if (online_id != 0)
        testBannedForOnlineId(peer.get(), online_id);
    // Will be disconnected if banned by online id
    if (peer->isDisconnected())
        return;

    unsigned total_players = 0;
    STKHost::get()->updatePlayers(NULL, NULL, &total_players);
    unsigned max_players_mode = (unsigned)ServerConfig::m_server_max_players;
    if (RaceManager::get()->getMinorMode() ==
        RaceManager::MINOR_MODE_FREE_FOR_ALL)
        max_players_mode = std::min<unsigned>(10, max_players_mode);
    if (RaceManager::get()->getMinorMode() ==
        RaceManager::MINOR_MODE_CAPTURE_THE_FLAG)
        max_players_mode = std::min<unsigned>(14, max_players_mode);
    if (RaceManager::get()->getMinorMode() ==
        RaceManager::MINOR_MODE_SOCCER)
        max_players_mode = std::min<unsigned>(14, max_players_mode);
    if (total_players + player_count + m_ai_profiles.size() >
        max_players_mode)
    {
        NetworkString *message = getNetworkString(2);
        message->setSynchronous(true);
        message->addUInt8(LE_CONNECTION_REFUSED).addUInt8(RR_TOO_MANY_PLAYERS);
        peer->sendPacket(message, true/*reliable*/, false/*encrypted*/);
        peer->reset();
        delete message;
        Log::verbose("ServerLobby", "Player refused: too many players");
        return;
    }

    // Reject non-valiated player joinning if WAN server and not disabled
    // encforement of validation, unless it's player from localhost or lan
    // And no duplicated online id or split screen players in ranked server
    // AIPeer only from lan and only 1 if ai handling
    std::set<uint32_t> all_online_ids =
        STKHost::get()->getAllPlayerOnlineIds();
    bool duplicated_ranked_player =
        all_online_ids.find(online_id) != all_online_ids.end();

    if (((encrypted_size == 0 || online_id == 0) &&
        !(peer->getAddress().isPublicAddressLocalhost() ||
        peer->getAddress().isLAN()) &&
        NetworkConfig::get()->isWAN() &&
        ServerConfig::m_validating_player) ||
        (ServerConfig::m_strict_players &&
        (player_count != 1 || online_id == 0 || duplicated_ranked_player)) ||
        (peer->isAIPeer() && !peer->getAddress().isLAN() &&!ServerConfig::m_ai_anywhere) ||
        (peer->isAIPeer() &&
        ServerConfig::m_ai_handling && !m_ai_peer.expired()) ||
        (peer->isAIPeer() && m_game_setup->isGrandPrix()))
    {
        NetworkString* message = getNetworkString(2);
        message->setSynchronous(true);
        message->addUInt8(LE_CONNECTION_REFUSED).addUInt8(RR_INVALID_PLAYER);
        peer->sendPacket(message, true/*reliable*/, false/*encrypted*/);
        peer->reset();
        delete message;
        Log::verbose("ServerLobby", "Player refused: invalid player");
        return;
    }

    if (ServerConfig::m_ai_handling && peer->isAIPeer())
        m_ai_peer = peer;
	
	if (ServerConfig::m_race_tournament) 
		peer->setAlwaysSpectate(true);

    if (encrypted_size != 0)
    {
        m_pending_connection[peer] = std::make_pair(online_id,
            BareNetworkString(data.getCurrentData(), encrypted_size));
    }
    else
    {
        core::stringw online_name;
        if (online_id > 0)
            data.decodeStringW(&online_name);
        handleUnencryptedConnection(peer, data, online_id, online_name,
            false/*is_pending_connection*/);
    }
}   // connectionRequested

//-----------------------------------------------------------------------------
void ServerLobby::handleUnencryptedConnection(std::shared_ptr<STKPeer> peer,
    BareNetworkString& data, uint32_t online_id,
    const core::stringw& online_name, bool is_pending_connection,
    std::string country_code)
{
    if (data.size() < 2) return;

	if (ServerConfig::m_race_tournament)
	{
		std::string username = StringUtils::wideToUtf8(online_name);
		bool found = m_race_tournament_players.find(username) != m_race_tournament_players.end();
		if (found)
			peer->setAlwaysSpectate(false);
	}

    // Check for password
    std::string password;
    data.decodeString(&password);
    const std::string& server_pw = ServerConfig::m_private_server_password;
    if (online_id > 0)
    {
        std::string username = StringUtils::wideToUtf8(online_name);
        if (m_temp_banned.count(username))
        {
            NetworkString* message = getNetworkString(2);
            message->setSynchronous(true);
            message->addUInt8(LE_CONNECTION_REFUSED)
                .addUInt8(RR_BANNED);
            std::string tempban = "You were banned from the server. Please behave well next time.";
            message->encodeString(tempban);
            peer->sendPacket(message, true/*reliable*/, false/*encrypted*/);
            peer->reset();
            delete message;
            Log::verbose("ServerLobby", "Player refused: invalid player");
            return;
        }
        if (m_usernames_white_list.count(username) > 0)
            password = server_pw;
    }
    if (password != server_pw)
    {
        NetworkString *message = getNetworkString(2);
        message->setSynchronous(true);
        message->addUInt8(LE_CONNECTION_REFUSED)
                .addUInt8(RR_INCORRECT_PASSWORD);
        peer->sendPacket(message, true/*reliable*/, false/*encrypted*/);
        peer->reset();
        delete message;
        Log::verbose("ServerLobby", "Player refused: incorrect password");
        return;
    }

    // Check again max players and duplicated player in ranked server,
    // if this is a pending connection
    unsigned total_players = 0;
    unsigned player_count = data.getUInt8();

    if (is_pending_connection)
    {
        STKHost::get()->updatePlayers(NULL, NULL, &total_players);
        if (total_players + player_count >
            (unsigned)ServerConfig::m_server_max_players)
        {
            NetworkString *message = getNetworkString(2);
            message->setSynchronous(true);
            message->addUInt8(LE_CONNECTION_REFUSED)
                .addUInt8(RR_TOO_MANY_PLAYERS);
            peer->sendPacket(message, true/*reliable*/, false/*encrypted*/);
            peer->reset();
            delete message;
            Log::verbose("ServerLobby", "Player refused: too many players");
            return;
        }

        std::set<uint32_t> all_online_ids =
            STKHost::get()->getAllPlayerOnlineIds();
        bool duplicated_ranked_player =
            all_online_ids.find(online_id) != all_online_ids.end();
        if (ServerConfig::m_ranked && duplicated_ranked_player)
        {
            NetworkString* message = getNetworkString(2);
            message->setSynchronous(true);
            message->addUInt8(LE_CONNECTION_REFUSED)
                .addUInt8(RR_INVALID_PLAYER);
            peer->sendPacket(message, true/*reliable*/, false/*encrypted*/);
            peer->reset();
            delete message;
            Log::verbose("ServerLobby", "Player refused: invalid player");
            return;
        }
    }

#ifdef ENABLE_SQLITE3
    if (country_code.empty() && !peer->getAddress().isIPv6())
        country_code = ip2Country(peer->getAddress());
    if (country_code.empty() && peer->getAddress().isIPv6())
        country_code = ipv62Country(peer->getAddress());
#endif

    auto red_blue = STKHost::get()->getAllPlayersTeamInfo();
    std::string utf8_online_name = StringUtils::wideToUtf8(online_name);
    for (unsigned i = 0; i < player_count; i++)
    {
        core::stringw name;
        data.decodeStringW(&name);
        // 30 to make it consistent with stk-addons max user name length
        if (name.empty())
            name = L"unnamed";
        else if (name.size() > 30)
            name = name.subString(0, 30);

        std::string utf8_name = StringUtils::wideToUtf8(name);
        float default_kart_color = data.getFloat();
        HandicapLevel handicap = (HandicapLevel)data.getUInt8();
        auto player = std::make_shared<NetworkPlayerProfile>
            (peer, i == 0 && !online_name.empty() && !peer->isAIPeer() ?
            online_name : name,
            peer->getHostId(), default_kart_color, i == 0 ? online_id : 0,
            handicap, (uint8_t)i, KART_TEAM_NONE,
            country_code);
        if (ServerConfig::m_team_choosing && !ServerConfig::m_soccer_tournament)
        {
            KartTeam cur_team = KART_TEAM_NONE;
            if (red_blue.first > red_blue.second)
            {
                cur_team = KART_TEAM_BLUE;
                red_blue.second++;
            }
            else
            {
                cur_team = KART_TEAM_RED;
                red_blue.first++;
            }
            player->setTeam(cur_team);
        }
        if (ServerConfig::m_soccer_tournament) {
            if (m_tournament_red_players.count(utf8_online_name)) 
                player->setTeam(KART_TEAM_RED);
            else if (m_tournament_blue_players.count(utf8_online_name))
                player->setTeam(KART_TEAM_BLUE);
            if (tournamentColorsSwapped(m_tournament_game))
            {
                if (player->getTeam() == KART_TEAM_BLUE)
                    player->setTeam(KART_TEAM_RED);
                else if (player->getTeam() == KART_TEAM_RED)
                    player->setTeam(KART_TEAM_BLUE);
            }
        }
        peer->addPlayer(player);
    }

    peer->setValidated(true);

	if (m_player_queue_limit > 0)
		addDeletePlayersFromQueue(peer, true);

    // send a message to the one that asked to connect
    NetworkString* server_info = getNetworkString();
    server_info->setSynchronous(true);
    server_info->addUInt8(LE_SERVER_INFO);
    m_game_setup->addServerInfo(server_info);
    peer->sendPacket(server_info);
    delete server_info;

    const bool game_started = m_state.load() != WAITING_FOR_START_GAME;
    NetworkString* message_ack = getNetworkString(4);
    message_ack->setSynchronous(true);
    // connection success -- return the host id of peer
    float auto_start_timer = 0.0f;
    if (m_timeout.load() == std::numeric_limits<int64_t>::max())
        auto_start_timer = std::numeric_limits<float>::max();
    else
    {
        auto_start_timer =
            (m_timeout.load() - (int64_t)StkTime::getMonoTimeMs()) / 1000.0f;
    }
    message_ack->addUInt8(LE_CONNECTION_ACCEPTED).addUInt32(peer->getHostId())
        .addUInt32(ServerConfig::m_server_version);

    message_ack->addUInt16(
        (uint16_t)stk_config->m_network_capabilities.size());
    for (const std::string& cap : stk_config->m_network_capabilities)
        message_ack->encodeString(cap);

    message_ack->addFloat(auto_start_timer)
        .addUInt32(ServerConfig::m_state_frequency)
        .addUInt8(ServerConfig::m_chat ? 1 : 0)
        .addUInt8(m_player_reports_table_exists ? 1 : 0);

    peer->setSpectator(false);

    // The 127.* or ::1/128 will be in charged for controlling AI
    if (m_ai_profiles.empty() && peer->getAddress().isLoopback())
    {
        unsigned ai_add = NetworkConfig::get()->getNumFixedAI();
        unsigned max_players = ServerConfig::m_server_max_players;
        // We need to reserve at least 1 slot for new player
        if (player_count + ai_add + 1 > max_players)
            ai_add = max_players - player_count - 1;
        for (unsigned i = 0; i < ai_add; i++)
        {
#ifdef SERVER_ONLY
            core::stringw name = L"Bot";
#else
            core::stringw name = _("Bot");
#endif
            if (i > 0)
                name += core::stringw(" ") + StringUtils::toWString(i);
            m_ai_profiles.push_back(std::make_shared<NetworkPlayerProfile>
                (peer, name, peer->getHostId(), 0.0f, 0, HANDICAP_NONE,
                player_count + i, KART_TEAM_NONE, ""));
        }
    }

    if (game_started)
    {
        peer->setWaitingForGame(true);
        updatePlayerList();
        peer->sendPacket(message_ack);
        delete message_ack;
    }
    else
    {
        peer->setWaitingForGame(false);
        m_peers_ready[peer] = false;
        if (!ServerConfig::m_sql_management)
        {
            for (std::shared_ptr<NetworkPlayerProfile>& npp :
                peer->getPlayerProfiles())
            {
                Log::info("ServerLobby",
                    "New player %s with online id %u from %s with %s.",
                    StringUtils::wideToUtf8(npp->getName()).c_str(),
                    npp->getOnlineId(), peer->getAddress().toString().c_str(),
                    peer->getUserVersion().c_str());
            }
        }
        updatePlayerList();
        peer->sendPacket(message_ack);
        delete message_ack;

        if (ServerConfig::m_ranked)
        {
            getRankingForPlayer(peer->getPlayerProfiles()[0]);
        }
    }

#ifdef ENABLE_SQLITE3
    if (m_server_stats_table.empty() || peer->isAIPeer())
        return;
    std::string query;
    if (ServerConfig::m_ipv6_connection && peer->getAddress().isIPv6())
    {
        query = StringUtils::insertValues(
            "INSERT INTO %s "
            "(host_id, ip, ipv6 ,port, online_id, username, player_num, "
            "country_code, version, os, ping, addon_karts_count, addon_tracks_count, "
            "addon_arenas_count, addon_soccers_count) "
            "VALUES (%u, 0, \"%s\" ,%u, %u, ?, %u, ?, ?, ?, %u, %d, %d, %d, %d);",
            m_server_stats_table.c_str(), peer->getHostId(),
            peer->getAddress().toString(false), peer->getAddress().getPort(),
            online_id, player_count, peer->getAveragePing(), peer->addon_karts_count,
            peer->addon_tracks_count, peer->addon_arenas_count,
            peer->addon_soccers_count);
    }
    else
    {
        query = StringUtils::insertValues(
            "INSERT INTO %s "
            "(host_id, ip, port, online_id, username, player_num, "
            "country_code, version, os, ping, addon_karts_count, addon_tracks_count, "
            "addon_arenas_count, addon_soccers_count) "
            "VALUES (%u, %u, %u, %u, ?, %u, ?, ?, ?, %u, %d, %d, %d, %d);",
            m_server_stats_table.c_str(), peer->getHostId(),
            peer->getAddress().getIP(), peer->getAddress().getPort(),
            online_id, player_count, peer->getAveragePing(), peer->addon_karts_count,
            peer->addon_tracks_count, peer->addon_arenas_count,
            peer->addon_soccers_count);
    }
    easySQLQuery(query, [peer, country_code](sqlite3_stmt* stmt)
        {
            if (sqlite3_bind_text(stmt, 1, StringUtils::wideToUtf8(
                peer->getPlayerProfiles()[0]->getName()).c_str(),
                -1, SQLITE_TRANSIENT) != SQLITE_OK)
            {
                Log::error("easySQLQuery", "Failed to bind %s.",
                    StringUtils::wideToUtf8(
                    peer->getPlayerProfiles()[0]->getName()).c_str());
            }
            if (country_code.empty())
            {
                if (sqlite3_bind_null(stmt, 2) != SQLITE_OK)
                {
                    Log::error("easySQLQuery",
                        "Failed to bind NULL for country code.");
                }
            }
            else
            {
                if (sqlite3_bind_text(stmt, 2, country_code.c_str(),
                    -1, SQLITE_TRANSIENT) != SQLITE_OK)
                {
                    Log::error("easySQLQuery", "Failed to bind country: %s.",
                        country_code.c_str());
                }
            }
            auto version_os =
                StringUtils::extractVersionOS(peer->getUserVersion());
            if (sqlite3_bind_text(stmt, 3, version_os.first.c_str(),
                -1, SQLITE_TRANSIENT) != SQLITE_OK)
            {
                Log::error("easySQLQuery", "Failed to bind %s.",
                    version_os.first.c_str());
            }
            if (sqlite3_bind_text(stmt, 4, version_os.second.c_str(),
                -1, SQLITE_TRANSIENT) != SQLITE_OK)
            {
                Log::error("easySQLQuery", "Failed to bind %s.",
                    version_os.second.c_str());
            }
        }
    );
#endif
    if (m_gnu_elimination)
    {
        NetworkString* chat = getNetworkString();
        chat->addUInt8(LE_CHAT);
        chat->setSynchronous(true);
        std::string gnu_warning = StringUtils::insertValues(
            "Gnu Elimination is played right now on this server, "
            "you will be forced to use kart %d until it ends. "
            "Use /standings to see the results.",
            m_gnu_kart);
        chat->encodeString16(StringUtils::utf8ToWide(gnu_warning));
        peer->sendPacket(chat, true/*reliable*/);
        delete chat;
    }
}   // handleUnencryptedConnection

//-----------------------------------------------------------------------------
/** Called when any players change their setting (team for example), or
 *  connection / disconnection, it will use the game_started parameter to
 *  determine if this should be send to all peers in server or just in game.
 *  \param update_when_reset_server If true, this message will be sent to
 *  all peers.
 */
void ServerLobby::updatePlayerList(bool update_when_reset_server)
{
    const bool game_started = m_state.load() != WAITING_FOR_START_GAME &&
        !update_when_reset_server;

    if (update_when_reset_server)
    {
        if (!ServerConfig::m_soccer_tournament && !ServerConfig::m_race_tournament)
        {
            for (auto& peer : m_default_always_spectate_peers)
                peer->setAlwaysSpectate(true);
        }
        else
        {
            auto peers = STKHost::get()->getPeers();

			if (ServerConfig::m_race_tournament)
			{
				for (auto peer : peers)
				{
					std::string username = StringUtils::wideToUtf8(peer->getPlayerProfiles()[0]->getName());
					bool found = m_race_tournament_players.find(username) != m_race_tournament_players.end();
					if (found)
						peer->setAlwaysSpectate(false);
					else
						peer->setAlwaysSpectate(true);
				}
			}
			else
			{
				for (auto& peer : peers)
				{
					peer->setAlwaysSpectate(false);
				}
			}
        }
    }

    auto all_profiles = STKHost::get()->getAllPlayerProfiles();
    size_t all_profiles_size = all_profiles.size();
    for (auto& profile : all_profiles)
    {
        if (profile->getPeer()->alwaysSpectate())
            all_profiles_size--;
    }
    // N - 1 AI
    auto ai_instance = m_ai_peer.lock();
    if (supportsAI())
    {
        if (ai_instance)
        {
            auto ai_profiles = ai_instance->getPlayerProfiles();
            if (m_state.load() == WAITING_FOR_START_GAME ||
                update_when_reset_server)
            {
                if (all_profiles_size > ai_profiles.size())
                    ai_profiles.clear();
                else if (all_profiles_size != 0)
                {
                    ai_profiles.resize(
                        ai_profiles.size() - all_profiles_size + 1);
                }
            }
            else
            {
                // Use fixed number of AI calculated when started game
                ai_profiles.resize(m_ai_count);
            }
            all_profiles.insert(all_profiles.end(), ai_profiles.begin(),
                ai_profiles.end());
        }
        else if (!m_ai_profiles.empty())
        {
            all_profiles.insert(all_profiles.end(), m_ai_profiles.begin(),
                m_ai_profiles.end());
        }
    }
    m_lobby_players.store((int)all_profiles.size());

    // No need to update player list (for started grand prix currently)
    if (!allowJoinedPlayersWaiting() &&
        m_state.load() > WAITING_FOR_START_GAME && !update_when_reset_server)
        return;

    NetworkString* pl = getNetworkString();
    pl->setSynchronous(true);
    pl->addUInt8(LE_UPDATE_PLAYER_LIST)
        .addUInt8((uint8_t)(game_started ? 1 : 0))
        .addUInt8((uint8_t)all_profiles.size());
    for (auto profile : all_profiles)
    {
        std::shared_ptr<STKPeer> p = profile->getPeer();
        // get OS information
        auto version_os = StringUtils::extractVersionOS(profile->getPeer()->getUserVersion());
        //bool angry_host = profile->getPeer()->isAngryHost();
		bool angry_host = isVIP(p);
        std::string os_type_str = version_os.second;
        auto profile_name = profile->getName();
        // Add a Mobile emoji for mobile OS
        if (ServerConfig::m_expose_mobile && 
            (os_type_str == "iOS" || os_type_str == "Android"))
            profile_name = StringUtils::utf32ToWide({0x1F4F1}) + profile_name;
        // Add a hammer emoji for angry host
        if (angry_host)
            profile_name = StringUtils::utf32ToWide({0x1F528}) + profile_name;
		if (m_player_queue_limit > 0)
		{
			auto p_name = StringUtils::wideToUtf8(profile->getName()); 
			stringw symbol = getQueueNumberIcon(p_name);
			profile_name = symbol + profile_name;
		}

        pl->addUInt32(profile->getHostId()).addUInt32(profile->getOnlineId())
            .addUInt8(profile->getLocalPlayerId())
            .encodeString(profile_name);

        uint8_t boolean_combine = 0;
        if (p && p->isWaitingForGame())
            boolean_combine |= 1;
        if (p && (p->isSpectator() ||
            ((m_state.load() == WAITING_FOR_START_GAME ||
            update_when_reset_server) && p->alwaysSpectate())))
            boolean_combine |= (1 << 1);
        if (p && m_server_owner_id.load() == p->getHostId())
            boolean_combine |= (1 << 2);
        if (ServerConfig::m_owner_less && !game_started &&
            m_peers_ready.find(p) != m_peers_ready.end() &&
            m_peers_ready.at(p))
            boolean_combine |= (1 << 3);
        if ((p && p->isAIPeer()) || isAIProfile(profile))
            boolean_combine |= (1 << 4);
        pl->addUInt8(boolean_combine);
        pl->addUInt8(profile->getHandicap());
        if (ServerConfig::m_team_choosing &&
            RaceManager::get()->teamEnabled())
            pl->addUInt8(profile->getTeam());
        else
            pl->addUInt8(KART_TEAM_NONE);
        pl->encodeString(profile->getCountryCode());
    }

    // Don't send this message to in-game players
    STKHost::get()->sendPacketToAllPeersWith([game_started]
        (STKPeer* p)
        {
            if (!p->isValidated())
                return false;
            if (!p->isWaitingForGame() && game_started)
                return false;
            return true;
        }, pl);
    delete pl;
}   // updatePlayerList

//-----------------------------------------------------------------------------
void ServerLobby::updateServerOwner()
{
    if (m_state.load() < WAITING_FOR_START_GAME ||
        m_state.load() > RESULT_DISPLAY ||
        ServerConfig::m_owner_less)
        return;
    if (!m_server_owner.expired())
        return;
    auto peers = STKHost::get()->getPeers();
    if (peers.empty())
        return;
    std::sort(peers.begin(), peers.end(), [](const std::shared_ptr<STKPeer> a,
        const std::shared_ptr<STKPeer> b)->bool
        {
            return a->getHostId() < b->getHostId();
        });

    std::shared_ptr<STKPeer> owner;
    for (auto peer: peers)
    {
        // Only matching host id can be server owner in case of
        // graphics-client-server
        if (peer->isValidated() && !peer->isAIPeer() &&
            (m_process_type == PT_MAIN ||
            peer->getHostId() == m_client_server_host_id.load()))
        {
            owner = peer;
            break;
        }
    }
    if (owner)
    {
        NetworkString* ns = getNetworkString();
        ns->setSynchronous(true);
        ns->addUInt8(LE_SERVER_OWNERSHIP);
        owner->sendPacket(ns);
        delete ns;
        m_server_owner = owner;
        m_server_owner_id.store(owner->getHostId());
        updatePlayerList();
    }
}   // updateServerOwner

//-----------------------------------------------------------------------------
/*! \brief Called when a player asks to select karts.
 *  \param event : Event providing the information.
 */
void ServerLobby::kartSelectionRequested(Event* event)
{
    if (m_state != SELECTING /*|| m_game_setup->isGrandPrixStarted()*/)
    {
        Log::warn("ServerLobby", "Received kart selection while in state %d.",
                  m_state.load());
        return;
    }

    if (!checkDataSize(event, 1) ||
        event->getPeer()->getPlayerProfiles().empty())
        return;

    const NetworkString& data = event->data();
    STKPeer* peer = event->getPeer();
    setPlayerKarts(data, peer);
}   // kartSelectionRequested

//-----------------------------------------------------------------------------
/*! \brief Called when a player votes for track(s), it will auto correct client
 *         data if it sends some invalid data.
 *  \param event : Event providing the information.
 */
void ServerLobby::handlePlayerVote(Event* event)
{
    if (m_state != SELECTING || !ServerConfig::m_track_voting)
    {
        Log::warn("ServerLobby", "Received track vote while in state %d.",
                  m_state.load());
        return;
    }

    if (!checkDataSize(event, 4) ||
        event->getPeer()->getPlayerProfiles().empty() ||
        event->getPeer()->isWaitingForGame())
        return;

    if (isVotingOver())  return;

    NetworkString& data = event->data();
    PeerVote vote(data);
    Log::debug("ServerLobby",
        "Vote from client: host %d, track %s, laps %d, reverse %d.",
        event->getPeer()->getHostId(), vote.m_track_name.c_str(),
        vote.m_num_laps, vote.m_reverse);

    Track* t = track_manager->getTrack(vote.m_track_name);
    if (!t)
    {
        vote.m_track_name = *m_available_kts.second.begin();
        t = track_manager->getTrack(vote.m_track_name);
        assert(t);
    }

    // Remove / adjust any invalid settings
    if (ServerConfig::m_soccer_tournament || ServerConfig::m_race_tournament) {
        vote.m_reverse = false;
    }
    else if (RaceManager::get()->modeHasLaps())
    {
        if (ServerConfig::m_auto_game_time_ratio > 0.0f)
        {
            vote.m_num_laps =
                (uint8_t)(fmaxf(1.0f, (float)t->getDefaultNumberOfLaps() *
                ServerConfig::m_auto_game_time_ratio));
        }
        else if (vote.m_num_laps == 0 || vote.m_num_laps > 20)
            vote.m_num_laps = (uint8_t)3;
        if (!t->reverseAvailable() && vote.m_reverse)
            vote.m_reverse = false;
    }
    else if (RaceManager::get()->isSoccerMode())
    {
        if (m_game_setup->isSoccerGoalTarget())
        {
            if (ServerConfig::m_auto_game_time_ratio > 0.0f)
            {
                vote.m_num_laps = (uint8_t)(ServerConfig::m_auto_game_time_ratio *
                                            UserConfigParams::m_num_goals);
            }
            else if (vote.m_num_laps > 10)
                vote.m_num_laps = (uint8_t)5;
        }
        else
        {
            if (ServerConfig::m_auto_game_time_ratio > 0.0f)
            {
                vote.m_num_laps = (uint8_t)(ServerConfig::m_auto_game_time_ratio *
                                            UserConfigParams::m_soccer_time_limit);
            }
            else if (vote.m_num_laps > 15)
                vote.m_num_laps = (uint8_t)7;
        }
    }
    else if (RaceManager::get()->getMinorMode() ==
        RaceManager::MINOR_MODE_FREE_FOR_ALL)
    {
        vote.m_num_laps = 0;
    }
    else if (RaceManager::get()->getMinorMode() ==
        RaceManager::MINOR_MODE_CAPTURE_THE_FLAG)
    {
        vote.m_num_laps = 0;
        vote.m_reverse = false;
    }
    if (m_fixed_lap >= 0) {
        vote.m_num_laps = m_fixed_lap;
    }

    // Store vote:
    vote.m_player_name = event->getPeer()->getPlayerProfiles()[0]->getName();
    addVote(event->getPeer()->getHostId(), vote);

    // Now inform all clients about the vote
    NetworkString other = NetworkString(PROTOCOL_LOBBY_ROOM);
    other.setSynchronous(true);
    other.addUInt8(LE_VOTE);
    other.addUInt32(event->getPeer()->getHostId());
    vote.encode(&other);
    sendMessageToPeers(&other);

}   // handlePlayerVote

// ----------------------------------------------------------------------------
/** Select the track to be used based on all votes being received.
 * \param winner_vote The PeerVote that was picked.
 * \param winner_peer_id The host id of winner (unchanged if no vote).
 *  \return True if race can go on, otherwise wait.
 */
bool ServerLobby::handleAllVotes(PeerVote* winner_vote,
                                 uint32_t* winner_peer_id)
{
    // Determine majority agreement when 35% of voting time remains,
    // reserve some time for kart selection so it's not 50%
    if (getRemainingVotingTime() / getMaxVotingTime() > 0.35f)
    {
        return false;
    }

    // First remove all votes from disconnected hosts
    auto it = m_peers_votes.begin();
    while (it != m_peers_votes.end())
    {
        auto peer = STKHost::get()->findPeerByHostId(it->first);
        if (peer == nullptr)
        {
            it = m_peers_votes.erase(it);
        }
        else
            it++;
    }

    if (m_peers_votes.empty())
    {
        if (isVotingOver())
        {
            *winner_vote = *m_default_vote;
            return true;
        }
        return false;
    }

    // Count number of players 
    float cur_players = 0.0f;
    auto peers = STKHost::get()->getPeers();
    for (auto peer : peers)
    {
        if (peer->isAIPeer())
            continue;
        if (peer->hasPlayerProfiles() && !peer->isWaitingForGame())
            cur_players += 1.0f;
    }

    std::string top_track = m_default_vote->m_track_name;
    int top_laps = m_default_vote->m_num_laps;
    bool top_reverse = m_default_vote->m_reverse;

    std::map<std::string, unsigned> tracks;
    std::map<unsigned, unsigned> laps;
    std::map<bool, unsigned> reverses;

    // Ratio to determine majority agreement
    float tracks_rate = 0.0f;
    float laps_rate = 0.0f;
    float reverses_rate = 0.0f;
    RandomGenerator rg;

    for (auto& p : m_peers_votes)
    {
        auto track_vote = tracks.find(p.second.m_track_name);
        if (track_vote == tracks.end())
            tracks[p.second.m_track_name] = 1;
        else
            track_vote->second++;
        auto lap_vote = laps.find(p.second.m_num_laps);
        if (lap_vote == laps.end())
            laps[p.second.m_num_laps] = 1;
        else
            lap_vote->second++;
        auto reverse_vote = reverses.find(p.second.m_reverse);
        if (reverse_vote == reverses.end())
            reverses[p.second.m_reverse] = 1;
        else
            reverse_vote->second++;
    }

    unsigned vote = 0;
    auto track_vote = tracks.begin();
    // rg.get(2) == 0 will allow not always the "less" in map get picked
    for (auto c_vote = tracks.begin(); c_vote != tracks.end(); c_vote++)
    {
        if (c_vote->second > vote ||
            (c_vote->second >= vote && rg.get(2) == 0))
        {
            vote = c_vote->second;
            track_vote = c_vote;
        }
    }
    if (track_vote != tracks.end())
    {
        top_track = track_vote->first;
        tracks_rate = float(track_vote->second) / cur_players;
    }

    vote = 0;
    auto lap_vote = laps.begin();
    for (auto c_vote = laps.begin(); c_vote != laps.end(); c_vote++)
    {
        if (c_vote->second > vote ||
            (c_vote->second >= vote && rg.get(2) == 0))
        {
            vote = c_vote->second;
            lap_vote = c_vote;
        }
    }
    if (lap_vote != laps.end())
    {
        top_laps = lap_vote->first;
        laps_rate = float(lap_vote->second) / cur_players;
    }

    vote = 0;
    auto reverse_vote = reverses.begin();
    for (auto c_vote = reverses.begin(); c_vote != reverses.end(); c_vote++)
    {
        if (c_vote->second > vote ||
            (c_vote->second >= vote && rg.get(2) == 0))
        {
            vote = c_vote->second;
            reverse_vote = c_vote;
        }
    }
    if (reverse_vote != reverses.end())
    {
        top_reverse = reverse_vote->first;
        reverses_rate = float(reverse_vote->second) / cur_players;
    }

    // End early if there is majority agreement which is all entries rate > 0.5
    it = m_peers_votes.begin();
    if (tracks_rate > 0.5f && laps_rate > 0.5f && reverses_rate > 0.5f)
    {
        while (it != m_peers_votes.end())
        {
            if (it->second.m_track_name == top_track &&
                it->second.m_num_laps == top_laps &&
                it->second.m_reverse == top_reverse)
                break;
            else
                it++;
        }
        if (it == m_peers_votes.end())
        {
            Log::warn("ServerLobby",
                "Missing track %s from majority.", top_track.c_str());
            it = m_peers_votes.begin();
        }
        *winner_peer_id = it->first;
        *winner_vote = it->second;
        return true;
    }
    else if (isVotingOver())
    {
        // Pick the best lap (or soccer goal / time) from only the top track
        // if no majority agreement from all
        int diff = std::numeric_limits<int>::max();
        auto closest_lap = m_peers_votes.begin();
        while (it != m_peers_votes.end())
        {
            if (it->second.m_track_name == top_track &&
                std::abs((int)it->second.m_num_laps - top_laps) < diff)
            {
                closest_lap = it;
                diff = std::abs((int)it->second.m_num_laps - top_laps);
            }
            else
                it++;
        }
        *winner_peer_id = closest_lap->first;
        *winner_vote = closest_lap->second;
        return true;
    }
    return false;
}   // handleAllVotes

// ----------------------------------------------------------------------------
void ServerLobby::getHitCaptureLimit()
{
    int hit_capture_limit = std::numeric_limits<int>::max();
    float time_limit = 0.0f;
    if (RaceManager::get()->getMinorMode() ==
        RaceManager::MINOR_MODE_CAPTURE_THE_FLAG)
    {
        if (ServerConfig::m_capture_limit > 0)
            hit_capture_limit = ServerConfig::m_capture_limit;
        if (ServerConfig::m_time_limit_ctf > 0)
            time_limit = (float)ServerConfig::m_time_limit_ctf;
    }
    else
    {
        if (ServerConfig::m_hit_limit > 0)
            hit_capture_limit = ServerConfig::m_hit_limit;
        if (ServerConfig::m_time_limit_ffa > 0.0f)
            time_limit = (float)ServerConfig::m_time_limit_ffa;
    }
    m_battle_hit_capture_limit = hit_capture_limit;
    m_battle_time_limit = time_limit;
}   // getHitCaptureLimit

void ServerLobby::init1vs1Ranking()
{
	if (ServerConfig::m_rank_1vs1 || ServerConfig::m_rank_1vs1_2 || ServerConfig::m_rank_1vs1_3)
	{
		std::vector<std::string> usernames;
		for (auto p : m_peers_ready)
		{
			auto peer = p.first.lock();
			if (!peer->isSpectator())
			{
				for (auto player : peer->getPlayerProfiles())
					usernames.push_back(std::string(StringUtils::wideToUtf8(player->getName())));
			}
		}

		if (usernames.size() == 2)
		{
			std::string suffix = ServerConfig::m_rank_1vs1 ? "1vs1" : (ServerConfig::m_rank_1vs1_2 ? "1vs1_2" : "1vs1_3");
			std::string singdrossel = "python3 current_1vs1_players.py " + usernames[0] + " " + usernames[1] + " " + suffix;
			system(singdrossel.c_str());
		}
		else
		{
			Log::warn("ServerLobby", "[1vs1] This game will not count for ranking since the number of players is %d (should be 2).", usernames.size());
			std::string users_str = "[1vs1] List of usernames:";
			for (auto &username : usernames) users_str += " " + username;
			Log::warn("ServerLobby", users_str.c_str());
			std::string message = "This game will not count for 1vs1 ranking, since the number of players is not equal to 2.";
			sendStringToAllPeers(message);
		}
	}
}

// ----------------------------------------------------------------------------
/** Called from the RaceManager of the server when the world is loaded. Marks
 *  the server to be ready to start the race.
 */
void ServerLobby::finishedLoadingWorld()
{
    for (auto p : m_peers_ready)
    {
        if (auto peer = p.first.lock())
            peer->updateLastActivity();
    }
    m_server_has_loaded_world.store(true);
}   // finishedLoadingWorld;

//-----------------------------------------------------------------------------
void add_gamescore2(std::string player_name)
{
    std::string singdrossel;
    if(ServerConfig::m_rank_1vs1 || ServerConfig::m_rank_1vs1_2 || ServerConfig::m_rank_1vs1_3) return;//singdrossel="python3 update_list.py "+player_name+" games 0 1vs1";
    else singdrossel="python3 update_list.py "+player_name+" games 0 3vs3";
    system(singdrossel.c_str());
}

void rem_gamescore2(std::string player_name, double phase)
{
    std::string ringdrossel;
    if(ServerConfig::m_rank_1vs1 || ServerConfig::m_rank_1vs1_2 || ServerConfig::m_rank_1vs1_3) return;//ringdrossel="python3 update_list.py "+player_name+" leftgame "+std::to_string(phase)+" 1vs1";
    else ringdrossel="python3 update_list.py "+player_name+" leftgame "+std::to_string(phase)+" 3vs3";
    system(ringdrossel.c_str());
}

/** Called when a client notifies the server that it has loaded the world.
 *  When all clients and the server are ready, the race can be started.
 */
void ServerLobby::finishedLoadingWorldClient(Event *event)
{
    std::shared_ptr<STKPeer> peer = event->getPeerSP();
    peer->updateLastActivity();
    m_peers_ready.at(peer) = true;
    Log::info("ServerLobby", "Peer %d has finished loading world at %lf",
        peer->getHostId(), StkTime::getRealTime());
    if (ServerConfig::m_save_goals)
    {
        std::string username = StringUtils::wideToUtf8(peer->getPlayerProfiles()[0]->getName());
        add_gamescore2(username);
    }
    if (ServerConfig::m_super_tournament && ServerConfig::m_count_supertournament_game)
    {
        std::string username = StringUtils::wideToUtf8(peer->getPlayerProfiles()[0]->getName());
        std::string redname=ServerConfig::m_red_team_name;
        std::string bluename=ServerConfig::m_blue_team_name;

        // Adding the current players to the database
        if (peer->hasPlayerProfiles())
        {
            std::string singdrossel = "";

            switch (peer->getPlayerProfiles()[0]->getTeam())
            {
                    case KART_TEAM_RED:
                            singdrossel = "python3 supertournament_addcurrentplayer.py " + username + " " + redname;
                            break;
                    case KART_TEAM_BLUE:
                            singdrossel = "python3 supertournament_addcurrentplayer.py " + username + " " + bluename;
                            break;
                    default:
                            break;
            }

            if (singdrossel != "")
                    system(singdrossel.c_str());
        }
    }
}   // finishedLoadingWorldClient

//-----------------------------------------------------------------------------
/** Called when a client clicks on 'ok' on the race result screen.
 *  If all players have clicked on 'ok', go back to the lobby.
 */
void ServerLobby::playerFinishedResult(Event *event)
{
    if (m_rs_state.load() == RS_ASYNC_RESET ||
        m_state.load() != RESULT_DISPLAY)
        return;
    std::shared_ptr<STKPeer> peer = event->getPeerSP();
    m_peers_ready.at(peer) = true;
}   // playerFinishedResult

//-----------------------------------------------------------------------------
bool ServerLobby::waitingForPlayers() const
{
    // if (m_game_setup->isGrandPrix() && m_game_setup->isGrandPrixStarted())
    //     return false;
    return m_state.load() >= WAITING_FOR_START_GAME;
}   // waitingForPlayers

//-----------------------------------------------------------------------------
void ServerLobby::handlePendingConnection()
{
    std::lock_guard<std::mutex> lock(m_keys_mutex);

    for (auto it = m_pending_connection.begin();
         it != m_pending_connection.end();)
    {
        auto peer = it->first.lock();
        if (!peer)
        {
            it = m_pending_connection.erase(it);
        }
        else
        {
            const uint32_t online_id = it->second.first;
            auto key = m_keys.find(online_id);
            if (key != m_keys.end() && key->second.m_tried == false)
            {
                try
                {
                    if (decryptConnectionRequest(peer, it->second.second,
                        key->second.m_aes_key, key->second.m_aes_iv, online_id,
                        key->second.m_name, key->second.m_country_code))
                    {
                        it = m_pending_connection.erase(it);
                        m_keys.erase(online_id);
                        continue;
                    }
                    else
                        key->second.m_tried = true;
                }
                catch (std::exception& e)
                {
                    Log::error("ServerLobby",
                        "handlePendingConnection error: %s", e.what());
                    key->second.m_tried = true;
                }
            }
            it++;
        }
    }
}   // handlePendingConnection

//-----------------------------------------------------------------------------
bool ServerLobby::decryptConnectionRequest(std::shared_ptr<STKPeer> peer,
    BareNetworkString& data, const std::string& key, const std::string& iv,
    uint32_t online_id, const core::stringw& online_name,
    const std::string& country_code)
{
    auto crypto = std::unique_ptr<Crypto>(new Crypto(
        Crypto::decode64(key), Crypto::decode64(iv)));
    if (crypto->decryptConnectionRequest(data))
    {
        peer->setCrypto(std::move(crypto));
        Log::info("ServerLobby", "%s validated",
            StringUtils::wideToUtf8(online_name).c_str());
        handleUnencryptedConnection(peer, data, online_id,
            online_name, true/*is_pending_connection*/, country_code);
        return true;
    }
    return false;
}   // decryptConnectionRequest

//-----------------------------------------------------------------------------
void ServerLobby::getRankingForPlayer(std::shared_ptr<NetworkPlayerProfile> p)
{
    int priority = Online::RequestManager::HTTP_MAX_PRIORITY;
    auto request = std::make_shared<Online::XMLRequest>(priority);
    NetworkConfig::get()->setUserDetails(request, "get-ranking");

    const uint32_t id = p->getOnlineId();
    request->addParameter("id", id);
    request->executeNow();

    const XMLNode* result = request->getXMLData();
    std::string rec_success;

    // Default result
    double score = 2000.0;
    double max_score = 2000.0;
    unsigned num_races = 0;
    if (result->get("success", &rec_success))
    {
        if (rec_success == "yes")
        {
            result->get("scores", &score);
            result->get("max-scores", &max_score);
            result->get("num-races-done", &num_races);
        }
        else
        {
            Log::error("ServerLobby", "No ranking info found for player %s.",
                StringUtils::wideToUtf8(p->getName()).c_str());
            // Kick the player to avoid his score being reset in case
            // connection to stk addons is broken
            auto peer = p->getPeer();
            if (peer)
            {
                peer->kick();
                return;
            }
        }
    }
    else
    {
        Log::error("ServerLobby", "No ranking info found for player %s.",
            StringUtils::wideToUtf8(p->getName()).c_str());
        auto peer = p->getPeer();
        if (peer)
        {
            peer->kick();
            return;
        }
    }
    m_ranked_players[id] = p;
    m_scores[id] = score;
    m_max_scores[id] = max_score;
    m_num_ranked_races[id] = num_races;
}   // getRankingForPlayer

//-----------------------------------------------------------------------------
void ServerLobby::submitRankingsToAddons()
{
    // No ranking for battle mode
    if (!RaceManager::get()->modeHasLaps())
        return;

    // ========================================================================
    class SubmitRankingRequest : public Online::XMLRequest
    {
    public:
        SubmitRankingRequest(uint32_t online_id, double scores,
                             double max_scores, unsigned num_races,
                             const std::string& country_code)
            : XMLRequest(Online::RequestManager::HTTP_MAX_PRIORITY)
        {
            addParameter("id", online_id);
            addParameter("scores", scores);
            addParameter("max-scores", max_scores);
            addParameter("num-races-done", num_races);
            addParameter("country-code", country_code);
        }
        virtual void afterOperation()
        {
            Online::XMLRequest::afterOperation();
            const XMLNode* result = getXMLData();
            std::string rec_success;
            if (!(result->get("success", &rec_success) &&
                rec_success == "yes"))
            {
                Log::error("ServerLobby", "Failed to submit scores.");
            }
        }
    };   // UpdatePlayerRankingRequest
    // ========================================================================

    for (unsigned i = 0; i < RaceManager::get()->getNumPlayers(); i++)
    {
        const uint32_t id = RaceManager::get()->getKartInfo(i).getOnlineId();
        auto request = std::make_shared<SubmitRankingRequest>
            (id, m_scores.at(id), m_max_scores.at(id),
            m_num_ranked_races.at(id),
            RaceManager::get()->getKartInfo(i).getCountryCode());
        NetworkConfig::get()->setUserDetails(request, "submit-ranking");
        Log::info("ServerLobby", "Submiting ranking for %s (%d) : %lf, %lf %d",
            StringUtils::wideToUtf8(
            RaceManager::get()->getKartInfo(i).getPlayerName()).c_str(), id,
            m_scores.at(id), m_max_scores.at(id), m_num_ranked_races.at(id));
        request->queue();
    }
}   // submitRankingsToAddons

//-----------------------------------------------------------------------------
/** This function is called when all clients have loaded the world and
 *  are therefore ready to start the race. It determine the start time in
 *  network timer for client and server based on pings and then switches state
 *  to WAIT_FOR_RACE_STARTED.
 */
void ServerLobby::configPeersStartTime()
{
    uint32_t max_ping = 0;
    const unsigned max_ping_from_peers = ServerConfig::m_max_ping;
    bool peer_exceeded_max_ping = false;
    for (auto p : m_peers_ready)
    {
        auto peer = p.first.lock();
        // Spectators don't send input so we don't need to delay for them
        if (!peer || peer->alwaysSpectate())
            continue;
        if (peer->getAveragePing() > max_ping_from_peers)
        {
            Log::warn("ServerLobby",
                "Peer %s cannot catch up with max ping %d.",
                peer->getAddress().toString().c_str(), max_ping);
            peer_exceeded_max_ping = true;
            continue;
        }
        max_ping = std::max(peer->getAveragePing(), max_ping);
    }
    if ((ServerConfig::m_high_ping_workaround && peer_exceeded_max_ping) ||
        (ServerConfig::m_live_players && RaceManager::get()->supportsLiveJoining()))
    {
        Log::info("ServerLobby", "Max ping to ServerConfig::m_max_ping for "
            "live joining or high ping workaround.");
        max_ping = ServerConfig::m_max_ping;
    }
    // Start up time will be after 2500ms, so even if this packet is sent late
    // (due to packet loss), the start time will still ahead of current time
    uint64_t start_time = STKHost::get()->getNetworkTimer() + (uint64_t)2500;
    powerup_manager->setRandomSeed(start_time);
    NetworkString* ns = getNetworkString(10);
    ns->setSynchronous(true);
    ns->addUInt8(LE_START_RACE).addUInt64(start_time);
    const uint8_t cc = (uint8_t)Track::getCurrentTrack()->getCheckManager()->getCheckStructureCount();
    ns->addUInt8(cc);
    *ns += *m_items_complete_state;
    m_client_starting_time = start_time;
    sendMessageToPeers(ns, /*reliable*/true);

    const unsigned jitter_tolerance = ServerConfig::m_jitter_tolerance;
    Log::info("ServerLobby", "Max ping from peers: %d, jitter tolerance: %d",
        max_ping, jitter_tolerance);
    // Delay server for max ping / 2 from peers and jitter tolerance.
    m_server_delay = (uint64_t)(max_ping / 2) + (uint64_t)jitter_tolerance;
    start_time += m_server_delay;
    m_server_started_at = start_time;
    delete ns;
    m_state = WAIT_FOR_RACE_STARTED;

    World::getWorld()->setPhase(WorldStatus::SERVER_READY_PHASE);
    // Different stk process thread may have different stk host
    STKHost* stk_host = STKHost::get();
    joinStartGameThread();
    m_start_game_thread = std::thread([start_time, stk_host, this]()
        {
            const uint64_t cur_time = stk_host->getNetworkTimer();
            assert(start_time > cur_time);
            int sleep_time = (int)(start_time - cur_time);
            //Log::info("ServerLobby", "Start game after %dms", sleep_time);
            StkTime::sleep(sleep_time);
            //Log::info("ServerLobby", "Started at %lf", StkTime::getRealTime());
            m_state.store(RACING);
        });
}   // configPeersStartTime

//-----------------------------------------------------------------------------
bool ServerLobby::allowJoinedPlayersWaiting() const
{
    return true; //!m_game_setup->isGrandPrix();
}   // allowJoinedPlayersWaiting

//-----------------------------------------------------------------------------
void ServerLobby::addWaitingPlayersToGame()
{
    auto all_profiles = STKHost::get()->getAllPlayerProfiles();
    for (auto& profile : all_profiles)
    {
        auto peer = profile->getPeer();
        if (!peer || !peer->isValidated())
            continue;

        peer->setWaitingForGame(false);
        peer->setSpectator(false);
        if (m_peers_ready.find(peer) == m_peers_ready.end())
        {
            m_peers_ready[peer] = false;
            if (!ServerConfig::m_sql_management)
            {
                Log::info("ServerLobby",
                    "New player %s with online id %u from %s with %s.",
                    StringUtils::wideToUtf8(profile->getName()).c_str(),
                    profile->getOnlineId(),
                    peer->getAddress().toString().c_str(),
                    peer->getUserVersion().c_str());
            }
        }
        uint32_t online_id = profile->getOnlineId();
        if (ServerConfig::m_ranked &&
            (m_ranked_players.find(online_id) == m_ranked_players.end() ||
            (m_ranked_players.find(online_id) != m_ranked_players.end() &&
            m_ranked_players.at(online_id).expired())))
        {
            getRankingForPlayer(peer->getPlayerProfiles()[0]);
        }
    }
    // Re-activiate the ai
    if (auto ai = m_ai_peer.lock())
        ai->setValidated(true);
}   // addWaitingPlayersToGame

//-----------------------------------------------------------------------------
void ServerLobby::resetServer()
{
    addWaitingPlayersToGame();
    resetPeersReady();
    updatePlayerList(true/*update_when_reset_server*/);
	m_player_queue_limit = ServerConfig::m_player_queue_limit;
    NetworkString* server_info = getNetworkString();
    server_info->setSynchronous(true);
    server_info->addUInt8(LE_SERVER_INFO);
    m_game_setup->addServerInfo(server_info);
    sendMessageToPeersInServer(server_info);
    delete server_info;
    setup();
    m_state = NetworkConfig::get()->isLAN() ?
        WAITING_FOR_START_GAME : REGISTER_SELF_ADDRESS;
}   // resetServer

//-----------------------------------------------------------------------------
void ServerLobby::testBannedForIP(STKPeer* peer) const
{
#ifdef ENABLE_SQLITE3
    if (!m_db || !m_ip_ban_table_exists)
        return;

    // Test for IPv4
    if (peer->getAddress().isIPv6())
        return;

    int row_id = -1;
    unsigned ip_start = 0;
    unsigned ip_end = 0;
    std::string query = StringUtils::insertValues(
        "SELECT rowid, ip_start, ip_end, reason, description FROM %s "
        "WHERE ip_start <= %u AND ip_end >= %u "
        "AND datetime('now') > datetime(starting_time) AND "
        "(expired_days is NULL OR datetime"
        "(starting_time, '+'||expired_days||' days') > datetime('now')) "
        "LIMIT 1;",
        ServerConfig::m_ip_ban_table.c_str(),
        peer->getAddress().getIP(), peer->getAddress().getIP());

    sqlite3_stmt* stmt = NULL;
    int ret = sqlite3_prepare_v2(m_db, query.c_str(), -1, &stmt, 0);
    if (ret == SQLITE_OK)
    {
        ret = sqlite3_step(stmt);
        if (ret == SQLITE_ROW)
        {
            row_id = sqlite3_column_int(stmt, 0);
            ip_start = (unsigned)sqlite3_column_int64(stmt, 1);
            ip_end = (unsigned)sqlite3_column_int64(stmt, 2);
            const char* reason = (char*)sqlite3_column_text(stmt, 3);
            const char* desc = (char*)sqlite3_column_text(stmt, 4);
            Log::info("ServerLobby", "%s banned by IP: %s "
                "(rowid: %d, description: %s).",
                peer->getAddress().toString().c_str(), reason, row_id, desc);
            kickPlayerWithReason(peer, reason);
        }
        ret = sqlite3_finalize(stmt);
        if (ret != SQLITE_OK)
        {
            Log::error("ServerLobby",
                "Error finalize database for query %s: %s",
                query.c_str(), sqlite3_errmsg(m_db));
        }
    }
    else
    {
        Log::error("ServerLobby", "Error preparing database for query %s: %s",
            query.c_str(), sqlite3_errmsg(m_db));
        return;
    }
    if (row_id != -1)
    {
        query = StringUtils::insertValues(
            "UPDATE %s SET trigger_count = trigger_count + 1, "
            "last_trigger = datetime('now') "
            "WHERE ip_start = %u AND ip_end = %u;",
            ServerConfig::m_ip_ban_table.c_str(), ip_start, ip_end);
        easySQLQuery(query);
    }
#endif
}   // testBannedForIP

//-----------------------------------------------------------------------------
void ServerLobby::testBannedForIPv6(STKPeer* peer) const
{
#ifdef ENABLE_SQLITE3
    if (!m_db || !m_ipv6_ban_table_exists)
        return;

    // Test for IPv6
    if (!peer->getAddress().isIPv6())
        return;

    int row_id = -1;
    std::string ipv6_cidr;
    std::string query = StringUtils::insertValues(
        "SELECT rowid, ipv6_cidr, reason, description FROM %s "
        "WHERE insideIPv6CIDR(ipv6_cidr, ?) = 1 "
        "AND datetime('now') > datetime(starting_time) AND "
        "(expired_days is NULL OR datetime"
        "(starting_time, '+'||expired_days||' days') > datetime('now')) "
        "LIMIT 1;",
        ServerConfig::m_ipv6_ban_table.c_str());

    sqlite3_stmt* stmt = NULL;
    int ret = sqlite3_prepare_v2(m_db, query.c_str(), -1, &stmt, 0);
    if (ret == SQLITE_OK)
    {
        if (sqlite3_bind_text(stmt, 1,
            peer->getAddress().toString(false).c_str(), -1, SQLITE_TRANSIENT)
            != SQLITE_OK)
        {
            Log::error("ServerLobby", "Error binding ipv6 addr for query: %s",
                sqlite3_errmsg(m_db));
        }

        ret = sqlite3_step(stmt);
        if (ret == SQLITE_ROW)
        {
            row_id = sqlite3_column_int(stmt, 0);
            ipv6_cidr = (char*)sqlite3_column_text(stmt, 1);
            const char* reason = (char*)sqlite3_column_text(stmt, 2);
            const char* desc = (char*)sqlite3_column_text(stmt, 3);
            Log::info("ServerLobby", "%s banned by IP: %s "
                "(rowid: %d, description: %s).",
                peer->getAddress().toString().c_str(), reason, row_id, desc);
            kickPlayerWithReason(peer, reason);
        }
        ret = sqlite3_finalize(stmt);
        if (ret != SQLITE_OK)
        {
            Log::error("ServerLobby",
                "Error finalize database for query %s: %s",
                query.c_str(), sqlite3_errmsg(m_db));
        }
    }
    else
    {
        Log::error("ServerLobby", "Error preparing database for query %s: %s",
            query.c_str(), sqlite3_errmsg(m_db));
        return;
    }
    if (row_id != -1)
    {
        query = StringUtils::insertValues(
            "UPDATE %s SET trigger_count = trigger_count + 1, "
            "last_trigger = datetime('now') "
            "WHERE ipv6_cidr = ?;", ServerConfig::m_ipv6_ban_table.c_str());
        easySQLQuery(query, [ipv6_cidr](sqlite3_stmt* stmt)
            {
                if (sqlite3_bind_text(stmt, 1, ipv6_cidr.c_str(),
                    -1, SQLITE_TRANSIENT) != SQLITE_OK)
                {
                    Log::error("easySQLQuery", "Failed to bind %s.",
                        ipv6_cidr.c_str());
                }
            });
    }
#endif
}   // testBannedForIPv6

//-----------------------------------------------------------------------------
void ServerLobby::testBannedForOnlineId(STKPeer* peer,
                                        uint32_t online_id) const
{
#ifdef ENABLE_SQLITE3
    if (!m_db || !m_online_id_ban_table_exists)
        return;

    int row_id = -1;
    std::string query = StringUtils::insertValues(
        "SELECT rowid, reason, description FROM %s "
        "WHERE online_id = %u "
        "AND datetime('now') > datetime(starting_time) AND "
        "(expired_days is NULL OR datetime"
        "(starting_time, '+'||expired_days||' days') > datetime('now')) "
        "LIMIT 1;",
        ServerConfig::m_online_id_ban_table.c_str(), online_id);

    sqlite3_stmt* stmt = NULL;
    int ret = sqlite3_prepare_v2(m_db, query.c_str(), -1, &stmt, 0);
    if (ret == SQLITE_OK)
    {
        ret = sqlite3_step(stmt);
        if (ret == SQLITE_ROW)
        {
            row_id = sqlite3_column_int(stmt, 0);
            const char* reason = (char*)sqlite3_column_text(stmt, 1);
            const char* desc = (char*)sqlite3_column_text(stmt, 2);
            Log::info("ServerLobby", "%s banned by online id: %s "
                "(online id: %u rowid: %d, description: %s).",
                peer->getAddress().toString().c_str(), reason, online_id,
                row_id, desc);
            kickPlayerWithReason(peer, reason);
        }
        ret = sqlite3_finalize(stmt);
        if (ret != SQLITE_OK)
        {
            Log::error("ServerLobby", "Error finalize database: %s",
                sqlite3_errmsg(m_db));
        }
    }
    else
    {
        Log::error("ServerLobby", "Error preparing database: %s",
            sqlite3_errmsg(m_db));
        return;
    }
    if (row_id != -1)
    {
        query = StringUtils::insertValues(
            "UPDATE %s SET trigger_count = trigger_count + 1, "
            "last_trigger = datetime('now') "
            "WHERE online_id = %u;",
            ServerConfig::m_online_id_ban_table.c_str(), online_id);
        easySQLQuery(query);
    }
#endif
}   // testBannedForOnlineId

//-----------------------------------------------------------------------------
void ServerLobby::listBanTable()
{
#ifdef ENABLE_SQLITE3
    if (!m_db)
        return;
    auto printer = [](void* data, int argc, char** argv, char** name)
        {
            for (int i = 0; i < argc; i++)
            {
                std::cout << name[i] << " = " << (argv[i] ? argv[i] : "NULL")
                    << "\n";
            }
            std::cout << "\n";
            return 0;
        };
    if (m_ip_ban_table_exists)
    {
        std::string query = "SELECT * FROM ";
        query += ServerConfig::m_ip_ban_table;
        query += ";";
        std::cout << "IP ban list:\n";
        sqlite3_exec(m_db, query.c_str(), printer, NULL, NULL);
    }
    if (m_online_id_ban_table_exists)
    {
        std::string query = "SELECT * FROM ";
        query += ServerConfig::m_online_id_ban_table;
        query += ";";
        std::cout << "Online Id ban list:\n";
        sqlite3_exec(m_db, query.c_str(), printer, NULL, NULL);
    }
#endif
}   // listBanTable

//-----------------------------------------------------------------------------
float ServerLobby::getStartupBoostOrPenaltyForKart(uint32_t ping,
                                                   unsigned kart_id)
{
    AbstractKart* k = World::getWorld()->getKart(kart_id);
    if (k->getStartupBoost() != 0.0f)
        return k->getStartupBoost();
    uint64_t now = STKHost::get()->getNetworkTimer();
    uint64_t client_time = now - ping / 2;
    uint64_t server_time = client_time + m_server_delay;
    int ticks = stk_config->time2Ticks(
        (float)(server_time - m_server_started_at) / 1000.0f);
    if (ticks < stk_config->time2Ticks(1.0f))
    {
        PlayerController* pc =
            dynamic_cast<PlayerController*>(k->getController());
        pc->displayPenaltyWarning();
        return -1.0f;
    }
    float f = k->getStartupBoostFromStartTicks(ticks);
    k->setStartupBoost(f);
    return f;
}   // getStartupBoostOrPenaltyForKart

//-----------------------------------------------------------------------------
/*! \brief Called when the server owner request to change game mode or
 *         difficulty.
 *  \param event : Event providing the information.
 *
 *  Format of the data :
 *  Byte 0            1            2
 *       -----------------------------------------------
 *  Size |     1      |     1     |         1          |
 *  Data | difficulty | game mode | soccer goal target |
 *       -----------------------------------------------
 */
void ServerLobby::handleServerConfiguration(Event* event)
{
    if (m_state != WAITING_FOR_START_GAME)
    {
        Log::warn("ServerLobby",
            "Received handleServerConfiguration while being in state %d.",
            m_state.load());
        return;
    }
    if (!ServerConfig::m_server_configurable)
    {
        Log::warn("ServerLobby", "server-configurable is not enabled.");
        // return;
    }
	if (event != NULL) /*&& event->getPeerSP() != m_server_owner.lock() ) */
	{
        auto peerSP = event->getPeerSP();
        if (!hasHostRights(peerSP))
        {
            Log::warn("ServerLobby",
            "Client %d is not authorised to config server.",
            event->getPeer()->getHostId());
            // return;
        }
	}
    int new_difficulty = ServerConfig::m_server_difficulty;
    int new_game_mode = ServerConfig::m_server_mode;
    bool new_soccer_goal_target = ServerConfig::m_soccer_goal_target;
    if (event != NULL)
    {
        NetworkString& data = event->data();
        new_difficulty = data.getUInt8();
        new_game_mode = data.getUInt8();
        new_soccer_goal_target = data.getUInt8() == 1;
    }
    unsigned total_players = 0;
    STKHost::get()->updatePlayers(NULL, NULL, &total_players);
    if ((new_game_mode == 6 && total_players > 14) ||
        (new_game_mode == 7 && total_players > 10) ||
        (new_game_mode == 8 && total_players > 14))
    {
        Log::error("ServerLobby", "Too many players (%d) to change mode to %d.",
            total_players, new_game_mode);
        auto peer = event->getPeerSP();
        std::string msg = "Too many players present to activate this mode. "
            "Soccer and CTF require at most 14, and FFA requires at most 10.";
        sendStringToPeer(msg, peer);
        return;
    }
    // Actually event == NULL implies no errors...
    if (event != NULL &&
        (m_available_difficulties.count(new_difficulty) == 0 || 
        m_available_modes.count(new_game_mode) == 0))
    {
        Log::error("ServerLobby", "Mode %d and/or difficulty %d are not permitted.");
        auto peer = event->getPeerSP();
        NetworkString* chat = getNetworkString();
        // I don't know for now which type to choose...
        chat->addUInt8(LE_CHAT);
        chat->setSynchronous(true);
        chat->encodeString16(L"Mode or difficulty are not permitted on this server");
        peer->sendPacket(chat, true/*reliable*/);
        delete chat;
        return;
    }
    auto modes = ServerConfig::getLocalGameMode(new_game_mode);
    if (modes.second == RaceManager::MAJOR_MODE_GRAND_PRIX)
    {
        Log::warn("ServerLobby", "Grand prix is used for new mode.");
        return;
    }

    RaceManager::get()->setMinorMode(modes.first);
    RaceManager::get()->setMajorMode(modes.second);
    RaceManager::get()->setDifficulty(RaceManager::Difficulty(new_difficulty));
    m_game_setup->resetExtraServerInfo();
    if (RaceManager::get()->getMinorMode() == RaceManager::MINOR_MODE_SOCCER)
        m_game_setup->setSoccerGoalTarget(new_soccer_goal_target);

    if (NetworkConfig::get()->isWAN() &&
        (m_difficulty.load() != new_difficulty ||
        m_game_mode.load() != new_game_mode))
    {
        Log::info("ServerLobby", "Updating server info with new "
            "difficulty: %d, game mode: %d to stk-addons.", new_difficulty,
            new_game_mode);
		m_set_field = "";
        int priority = Online::RequestManager::HTTP_MAX_PRIORITY;
        auto request = std::make_shared<Online::XMLRequest>(priority);
        NetworkConfig::get()->setServerDetails(request, "update-config");
        const SocketAddress& addr = STKHost::get()->getPublicAddress();
        request->addParameter("address", addr.getIP());
        request->addParameter("port", addr.getPort());
        request->addParameter("new-difficulty", new_difficulty);
        request->addParameter("new-game-mode", new_game_mode);
        request->queue();
    }
    m_difficulty.store(new_difficulty);
    m_game_mode.store(new_game_mode);
    updateTracksForMode();

    auto peers = STKHost::get()->getPeers();
    for (auto& peer : peers)
    {
        auto assets = peer->getClientAssets();
        if (!peer->isValidated() || assets.second.empty())
            continue;
        std::set<std::string> tracks_erase;
        for (const std::string& server_track : m_available_kts.second)
        {
            if (assets.second.find(server_track) == assets.second.end())
            {
                tracks_erase.insert(server_track);
            }
        }
        if (tracks_erase.size() == m_available_kts.second.size())
        {
            NetworkString *message = getNetworkString(2);
            message->setSynchronous(true);
            message->addUInt8(LE_CONNECTION_REFUSED)
                .addUInt8(RR_INCOMPATIBLE_DATA);
            peer->cleanPlayerProfiles();
            peer->sendPacket(message, true/*reliable*/);
            peer->reset();
            delete message;
            Log::verbose("ServerLobby",
                "Player has incompatible tracks for new game mode.");
        }
    }
    NetworkString* server_info = getNetworkString();
    server_info->setSynchronous(true);
    server_info->addUInt8(LE_SERVER_INFO);
    m_game_setup->addServerInfo(server_info);
    sendMessageToPeers(server_info);
    delete server_info;
    updatePlayerList();

    if (m_gnu_elimination &&
        RaceManager::get()->getMinorMode() != RaceManager::MINOR_MODE_NORMAL_RACE &&
        RaceManager::get()->getMinorMode() != RaceManager::MINOR_MODE_TIME_TRIAL)
    {
        m_gnu_elimination = false;
        m_gnu_remained = 0;
        m_gnu_participants.clear();
        NetworkString* chat = getNetworkString();
        chat->addUInt8(LE_CHAT);
        chat->setSynchronous(true);
        chat->encodeString16(
                L"Gnu Elimination is disabled because of non-racing mode");
        sendMessageToPeers(chat);
        delete chat;
    }
}   // handleServerConfiguration

//-----------------------------------------------------------------------------
/*! \brief Called when a player want to change his handicap
 *  \param event : Event providing the information.
 *
 *  Format of the data :
 *  Byte 0                 1
 *       ----------------------------------
 *  Size |       1         |       1      |
 *  Data | local player id | new handicap |
 *       ----------------------------------
 */
void ServerLobby::changeHandicap(Event* event)
{
    NetworkString& data = event->data();
    if (m_state.load() != WAITING_FOR_START_GAME &&
        !event->getPeer()->isWaitingForGame())
    {
        Log::warn("ServerLobby", "Set handicap at wrong time.");
        return;
    }
    uint8_t local_id = data.getUInt8();
    auto& player = event->getPeer()->getPlayerProfiles().at(local_id);
    uint8_t handicap_id = data.getUInt8();
    if (handicap_id >= HANDICAP_COUNT)
    {
        Log::warn("ServerLobby", "Wrong handicap %d.", handicap_id);
        return;
    }
    HandicapLevel h = (HandicapLevel)handicap_id;
    player->setHandicap(h);
    updatePlayerList();
}   // changeHandicap

//-----------------------------------------------------------------------------
/** Update and see if any player disconnects, if so eliminate the kart in
 *  world, so this function must be called in main thread.
 */
void ServerLobby::handlePlayerDisconnection() const
{
    if (!World::getWorld() ||
        World::getWorld()->getPhase() < WorldStatus::MUSIC_PHASE)
    {
        return;
    }

    int red_count = 0;
    int blue_count = 0;
    unsigned total = 0;
    for (unsigned i = 0; i < RaceManager::get()->getNumPlayers(); i++)
    {
        RemoteKartInfo& rki = RaceManager::get()->getKartInfo(i);
        if (rki.isReserved())
            continue;
        bool disconnected = rki.disconnected();
        if (RaceManager::get()->getKartInfo(i).getKartTeam() == KART_TEAM_RED &&
            !disconnected)
            red_count++;
        else if (RaceManager::get()->getKartInfo(i).getKartTeam() ==
            KART_TEAM_BLUE && !disconnected)
            blue_count++;

        if (!disconnected)
        {
            total++;
            continue;
        }
        else
            rki.makeReserved();

        AbstractKart* k = World::getWorld()->getKart(i);
        if (!k->isEliminated() && !k->hasFinishedRace())
        {
            CaptureTheFlag* ctf = dynamic_cast<CaptureTheFlag*>
                (World::getWorld());
            if (ctf)
                ctf->loseFlagForKart(k->getWorldKartId());

            World::getWorld()->eliminateKart(i,
                false/*notify_of_elimination*/);
            k->setPosition(
                World::getWorld()->getCurrentNumKarts() + 1);
            k->finishedRace(World::getWorld()->getTime(), true/*from_server*/);
        }
    }

    // If live players is enabled, don't end the game if unfair team
    if (!ServerConfig::m_live_players &&
        total != 1 && World::getWorld()->hasTeam() &&
        (red_count == 0 || blue_count == 0))
        World::getWorld()->setUnfairTeam(true);

}   // handlePlayerDisconnection

//-----------------------------------------------------------------------------
/** Add reserved players for live join later if required.
 */
void ServerLobby::addLiveJoinPlaceholder(
    std::vector<std::shared_ptr<NetworkPlayerProfile> >& players) const
{
    if (!ServerConfig::m_live_players || !RaceManager::get()->supportsLiveJoining())
        return;
    if (RaceManager::get()->getMinorMode() == RaceManager::MINOR_MODE_FREE_FOR_ALL)
    {
        Track* t = track_manager->getTrack(m_game_setup->getCurrentTrack());
        assert(t);
        int max_players = std::min((int)ServerConfig::m_server_max_players,
            (int)t->getMaxArenaPlayers());
        int add_size = max_players - (int)players.size();
        assert(add_size >= 0);
        for (int i = 0; i < add_size; i++)
        {
            players.push_back(
                NetworkPlayerProfile::getReservedProfile(KART_TEAM_NONE));
        }
    }
    else
    {
        // CTF or soccer, reserve at most 7 players on each team
        int red_count = 0;
        int blue_count = 0;
        for (unsigned i = 0; i < players.size(); i++)
        {
            if (players[i]->getTeam() == KART_TEAM_RED)
                red_count++;
            else
                blue_count++;
        }
        red_count = red_count >= 7 ? 0 : 7 - red_count;
        blue_count = blue_count >= 7 ? 0 : 7 - blue_count;
        for (int i = 0; i < red_count; i++)
        {
            players.push_back(
                NetworkPlayerProfile::getReservedProfile(KART_TEAM_RED));
        }
        for (int i = 0; i < blue_count; i++)
        {
            players.push_back(
                NetworkPlayerProfile::getReservedProfile(KART_TEAM_BLUE));
        }
    }
}   // addLiveJoinPlaceholder

//-----------------------------------------------------------------------------
void ServerLobby::setPlayerKarts(const NetworkString& ns, STKPeer* peer) const
{
    unsigned player_count = ns.getUInt8();
    for (unsigned i = 0; i < player_count; i++)
    {
        std::string kart;
        ns.decodeString(&kart);
        if (kart.find("randomkart") != std::string::npos ||
            (kart.find("addon_") == std::string::npos &&
            m_available_kts.first.find(kart) == m_available_kts.first.end()))
        {
            RandomGenerator rg;
            std::set<std::string>::iterator it =
                m_available_kts.first.begin();
            std::advance(it,
                rg.get((int)m_available_kts.first.size()));
            peer->getPlayerProfiles()[i]->setKartName(*it);
        }
        else
        {
			std::string username = StringUtils::wideToUtf8(peer->getPlayerProfiles()[i]->getName());

			if (m_set_kart.count(username) == 0)
				peer->getPlayerProfiles()[i]->setKartName(kart);
			else
				peer->getPlayerProfiles()[i]->setKartName(m_set_kart.at(username));
        }
    }
}   // setPlayerKarts

//-----------------------------------------------------------------------------
/** Tell the client \ref RemoteKartInfo of a player when some player joining
 *  live.
 */
void ServerLobby::handleKartInfo(Event* event)
{
    World* w = World::getWorld();
    if (!w)
        return;

    STKPeer* peer = event->getPeer();
    const NetworkString& data = event->data();
    uint8_t kart_id = data.getUInt8();
    if (kart_id > RaceManager::get()->getNumPlayers())
        return;

    AbstractKart* k = w->getKart(kart_id);
    int live_join_util_ticks = k->getLiveJoinUntilTicks();

    const RemoteKartInfo& rki = RaceManager::get()->getKartInfo(kart_id);

    NetworkString* ns = getNetworkString(1);
    ns->setSynchronous(true);
    ns->addUInt8(LE_KART_INFO).addUInt32(live_join_util_ticks)
        .addUInt8(kart_id) .encodeString(rki.getPlayerName())
        .addUInt32(rki.getHostId()).addFloat(rki.getDefaultKartColor())
        .addUInt32(rki.getOnlineId()).addUInt8(rki.getHandicap())
        .addUInt8((uint8_t)rki.getLocalPlayerId())
        .encodeString(rki.getKartName()).encodeString(rki.getCountryCode());
    peer->sendPacket(ns, true/*reliable*/);
    delete ns;
}   // handleKartInfo

//-----------------------------------------------------------------------------
/** Client if currently in-game (including spectator) wants to go back to
 *  lobby.
 */
void ServerLobby::clientInGameWantsToBackLobby(Event* event)
{
    World* w = World::getWorld();
    std::shared_ptr<STKPeer> peer = event->getPeerSP();

    if (!w || !worldIsActive() || peer->isWaitingForGame())
    {
        Log::warn("ServerLobby", "%s try to leave the game at wrong time.",
            peer->getAddress().toString().c_str());
        return;
    }

    if (m_process_type == PT_CHILD &&
        event->getPeer()->getHostId() == m_client_server_host_id.load())
    {
        // For child server the remaining client cannot go on player when the
        // server owner quited the game (because the world will be deleted), so
        // we reset all players
        auto pm = ProtocolManager::lock();
        if (RaceEventManager::get())
        {
            RaceEventManager::get()->stop();
            pm->findAndTerminate(PROTOCOL_GAME_EVENTS);
        }
        auto gp = GameProtocol::lock();
        if (gp)
        {
            auto lock = gp->acquireWorldDeletingMutex();
            pm->findAndTerminate(PROTOCOL_CONTROLLER_EVENTS);
            exitGameState();
        }
        else
            exitGameState();
        NetworkString* back_to_lobby = getNetworkString(2);
        back_to_lobby->setSynchronous(true);
        back_to_lobby->addUInt8(LE_BACK_LOBBY)
            .addUInt8(BLR_SERVER_ONWER_QUITED_THE_GAME);
        sendMessageToPeersInServer(back_to_lobby, /*reliable*/true);
        delete back_to_lobby;
        m_rs_state.store(RS_ASYNC_RESET);
        return;
    }

    for (const int id : peer->getAvailableKartIDs())
    {
        RemoteKartInfo& rki = RaceManager::get()->getKartInfo(id);
        if (rki.getHostId() == peer->getHostId())
        {
            Log::info("ServerLobby", "%s left the game with kart id %d.",
                peer->getAddress().toString().c_str(), id);
            rki.setNetworkPlayerProfile(
                std::shared_ptr<NetworkPlayerProfile>());
            if (ServerConfig::m_save_goals)
            {
                std::string username = StringUtils::wideToUtf8(peer->getPlayerProfiles()[0]->getName());
                double phase = 0.0;
                if (RaceManager::get()->hasTimeTarget())
                {
                    phase = (RaceManager::get()->getTimeTarget() - World::getWorld()->getTime())/RaceManager::get()->getTimeTarget();
                }
                else
                {
                    int red_scorers_count = 0; int blue_scorers_count = 0;
                    SoccerWorld *sw = dynamic_cast<SoccerWorld*>(World::getWorld());
                    if (sw)
                    {
                        red_scorers_count = sw->get_red_scorers_count();
                        blue_scorers_count = sw->get_blue_scorers_count();
                    }
                    phase = 1.0*std::max(red_scorers_count, blue_scorers_count)/RaceManager::get()->getMaxGoal();
                    std::string message = "red_scorers_cnt=" + std::to_string(red_scorers_count) + " / blue_scorers_cnt" + std::to_string(blue_scorers_count);
                    message += " / max_goll=" + std::to_string(RaceManager::get()->getMaxGoal());
                    Log::info("ServerLobby", message.c_str());
                }
                std::string message = "phase=" + std::to_string(phase);
                Log::info("ServerLobby", message.c_str());
                rem_gamescore2(username,phase);
            }
        }
        else
        {
            Log::error("ServerLobby", "%s doesn't exist anymore in server.",
                peer->getAddress().toString().c_str());
        }
    }
    NetworkItemManager* nim = dynamic_cast<NetworkItemManager*>
        (Track::getCurrentTrack()->getItemManager());
    assert(nim);
    nim->erasePeerInGame(peer);
    m_peers_ready.erase(peer);
    peer->setWaitingForGame(true);
    peer->setSpectator(false);

    NetworkString* reset = getNetworkString(2);
    reset->setSynchronous(true);
    reset->addUInt8(LE_BACK_LOBBY).addUInt8(BLR_NONE);
    peer->sendPacket(reset, /*reliable*/true);
    delete reset;
    updatePlayerList();
    NetworkString* server_info = getNetworkString();
    server_info->setSynchronous(true);
    server_info->addUInt8(LE_SERVER_INFO);
    m_game_setup->addServerInfo(server_info);
    peer->sendPacket(server_info, /*relsiable*/true);
    delete server_info;
}   // clientInGameWantsToBackLobby

//-----------------------------------------------------------------------------
/** Client if currently select assets wants to go back to lobby.
 */
void ServerLobby::clientSelectingAssetsWantsToBackLobby(Event* event)
{
    std::shared_ptr<STKPeer> peer = event->getPeerSP();

    if (m_state.load() != SELECTING || peer->isWaitingForGame())
    {
        Log::warn("ServerLobby",
            "%s try to leave selecting assets at wrong time.",
            peer->getAddress().toString().c_str());
        return;
    }

    if (m_process_type == PT_CHILD &&
        event->getPeer()->getHostId() == m_client_server_host_id.load())
    {
        NetworkString* back_to_lobby = getNetworkString(2);
        back_to_lobby->setSynchronous(true);
        back_to_lobby->addUInt8(LE_BACK_LOBBY)
            .addUInt8(BLR_SERVER_ONWER_QUITED_THE_GAME);
        sendMessageToPeersInServer(back_to_lobby, /*reliable*/true);
        delete back_to_lobby;
        resetVotingTime();
        resetServer();
        m_rs_state.store(RS_NONE);
        return;
    }

    m_peers_ready.erase(peer);
    peer->setWaitingForGame(true);
    peer->setSpectator(false);

    NetworkString* reset = getNetworkString(2);
    reset->setSynchronous(true);
    reset->addUInt8(LE_BACK_LOBBY).addUInt8(BLR_NONE);
    peer->sendPacket(reset, /*reliable*/true);
    delete reset;
    updatePlayerList();
    NetworkString* server_info = getNetworkString();
    server_info->setSynchronous(true);
    server_info->addUInt8(LE_SERVER_INFO);
    m_game_setup->addServerInfo(server_info);
    peer->sendPacket(server_info, /*reliable*/true);
    delete server_info;
}   // clientSelectingAssetsWantsToBackLobby

//-----------------------------------------------------------------------------
void ServerLobby::saveInitialItems(std::shared_ptr<NetworkItemManager> nim)
{
    m_items_complete_state->getBuffer().clear();
    m_items_complete_state->reset();
    nim->saveCompleteState(m_items_complete_state);
}   // saveInitialItems

//-----------------------------------------------------------------------------
bool ServerLobby::supportsAI()
{
    return getGameMode() == 3 || getGameMode() == 4;
}   // supportsAI

//-----------------------------------------------------------------------------
bool ServerLobby::checkPeersReady(bool ignore_ai_peer) const
{
    bool all_ready = true;
    bool someone_races = false;
    for (auto p : m_peers_ready)
    {
        auto peer = p.first.lock();
        if (!peer)
            continue;
        if (!canRace(peer))
            continue;
        if (ignore_ai_peer && peer->isAIPeer())
            continue;
        someone_races = true;
        all_ready = all_ready && p.second;
        if (!all_ready)
            return false;
    }
    return someone_races;
}   // checkPeersReady

//-----------------------------------------------------------------------------
void ServerLobby::handleServerCommand(Event* event,
                                      std::shared_ptr<STKPeer> peer)
{
    NetworkString& data = event->data();
    std::string language;
    data.decodeString(&language);
    std::string cmd;
    data.decodeString(&cmd);
    auto argv = StringUtils::split(cmd, ' ');
    if (argv.size() == 0)
        return;
    
    bool hostRights = hasHostRights(peer);
	std::string peer_username = StringUtils::wideToUtf8(
		peer->getPlayerProfiles()[0]->getName());

	// Even if a player has host rights, he can be fair and vote for a command.
	// Example: /vote gnu nolok
	if (argv[0] == "vote")
	{
		if (ServerConfig::m_command_voting == false)
		{
			std::string msg = "Command voting is disabled on this server.";
			sendStringToPeer(msg, peer);
			return;
		}
		else if (argv.size() < 2)
		{
			std::string msg = "Usage: /vote [command]";
			sendStringToPeer(msg, peer);
			return;
		}

		hostRights = false;

		// remove "vote " from the command
		argv.erase(argv.begin());
		cmd = cmd.substr(5, cmd.length());
	}

    if (argv[0] == "spectate")
    {
        if (ServerConfig::m_soccer_tournament || ServerConfig::m_only_host_riding || ServerConfig::m_race_tournament)
        {
            std::string msg = "All spectators already have auto spectate ability";
            sendStringToPeer(msg, peer);
            return;
        }
        if (/*m_game_setup->isGrandPrix() || */!ServerConfig::m_live_players)
        {
            std::string msg = "Server doesn't support spectate";
            sendStringToPeer(msg, peer);
            return;
        }

        if (argv.size() != 2 || (argv[1] != "0" && argv[1] != "1"))
        {
            std::string msg = "Usage: spectate [0 or 1], before game started";
            sendStringToPeer(msg, peer);
            return;
        }

        if (m_state.load() != WAITING_FOR_START_GAME)
        {
            if (argv[1] == "1")
                m_default_always_spectate_peers.insert(peer.get());
            else
                m_default_always_spectate_peers.erase(peer.get());

			if (m_player_queue_limit > 0)
				addDeletePlayersFromQueue(peer, argv[1] == "0");
			
            return;
        }

        if (argv[1] == "1")
        {
            if (m_process_type == PT_CHILD &&
                peer->getHostId() == m_client_server_host_id.load())
            {
                std::string msg = "Graphical client server cannot spectate";
                sendStringToPeer(msg, peer);
                return;
            }
            peer->setAlwaysSpectate(true);
        }
        else
            peer->setAlwaysSpectate(false);

		if (m_player_queue_limit > 0)
			addDeletePlayersFromQueue(peer, argv[1] == "0");

        updatePlayerList();
    }
    if (argv[0] == "listserveraddon")
    {
        NetworkString* chat = getNetworkString();
        chat->addUInt8(LE_CHAT);
        chat->setSynchronous(true);
        bool has_options = argv.size() > 1 &&
            (argv[1].compare("-track") == 0 ||
            argv[1].compare("-arena") == 0 ||
            argv[1].compare("-kart") == 0 ||
            argv[1].compare("-soccer") == 0);
        if (argv.size() == 1 || argv.size() > 3 || argv[1].size() < 3 ||
            (argv.size() == 2 && (argv[1].size() < 3 || has_options)) ||
            (argv.size() == 3 && (!has_options || argv[2].size() < 3)))
        {
            chat->encodeString16(
                L"Usage: /listserveraddon [option][addon string to find "
                "(at least 3 characters)]. Available options: "
                "-track, -arena, -kart, -soccer.");
        }
        else
        {
            std::string type = "";
            std::string text = "";
            if(argv.size() > 1)
            {
                if(argv[1].compare("-track") == 0 ||
                   argv[1].compare("-arena") == 0 ||
                   argv[1].compare("-kart" ) == 0 ||
                   argv[1].compare("-soccer" ) == 0)
                    type = argv[1].substr(1);
                if((argv.size() == 2 && type.empty()) || argv.size() == 3)
                    text = argv[argv.size()-1];
            }

            std::set<std::string> total_addons;
            if (type.empty() || // not specify addon type
               (!type.empty() && type.compare("kart") == 0)) // list kart addon
            {
                total_addons.insert(m_addon_kts.first.begin(), m_addon_kts.first.end());
            }
            if (type.empty() || // not specify addon type
               (!type.empty() && type.compare("track") == 0))
            {
                total_addons.insert(m_addon_kts.second.begin(), m_addon_kts.second.end());
            }
            if (type.empty() || // not specify addon type
               (!type.empty() && type.compare("arena") == 0))
            {
                total_addons.insert(m_addon_arenas.begin(), m_addon_arenas.end());
            }
            if (type.empty() || // not specify addon type
               (!type.empty() && type.compare("soccer") == 0))
            {
                total_addons.insert(m_addon_soccers.begin(), m_addon_soccers.end());
            }
            std::string msg = "";
            for (auto& addon : total_addons)
            {
                // addon_ (6 letters)
                if (!text.empty() && addon.find(text, 6) == std::string::npos)
                    continue;

                msg += addon.substr(6);
                msg += ", ";
            }
            if (msg.empty())
                chat->encodeString16(L"Addon not found");
            else
            {
                msg = msg.substr(0, msg.size() - 2);
                chat->encodeString16(StringUtils::utf8ToWide(
                    std::string("Server addon: ") + msg));
            }
        }
        peer->sendPacket(chat, true/*reliable*/);
        delete chat;
    }
    if (StringUtils::startsWith(cmd, "playerhasaddon"))
    {
        NetworkString* chat = getNetworkString();
        chat->addUInt8(LE_CHAT);
        chat->setSynchronous(true);
        std::string part;
        if (cmd.length() > 15)
            part = cmd.substr(15);
        std::string addon_id = part.substr(0, part.find(' '));
        std::string player_name;
        if (part.length() > addon_id.length() + 1)
            player_name = part.substr(addon_id.length() + 1);
        std::shared_ptr<STKPeer> player_peer = STKHost::get()->findPeerByName(
            StringUtils::utf8ToWide(player_name));
        if (player_name.empty() || !player_peer || addon_id.empty())
        {
            chat->encodeString16(
                L"Usage: /playerhasaddon [addon_identity] [player name]");
        }
        else
        {
            std::string addon_id_test = Addon::createAddonId(addon_id);
            bool found = false;
            const auto& kt = player_peer->getClientAssets();
            for (auto& kart : kt.first)
            {
                if (kart == addon_id_test)
                {
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                for (auto& track : kt.second)
                {
                    if (track == addon_id_test)
                    {
                        found = true;
                        break;
                    }
                }
            }
            if (found)
            {
                chat->encodeString16(StringUtils::utf8ToWide
                    (player_name + " has addon " + addon_id));
            }
            else
            {
                chat->encodeString16(StringUtils::utf8ToWide
                    (player_name + " has no addon " + addon_id));
            }
        }
        peer->sendPacket(chat, true/*reliable*/);
        delete chat;
    }
    if (StringUtils::startsWith(cmd, "kick"))
    {
        std::string player_name;
        if (StringUtils::startsWith(cmd, "kickban"))
        {
            if (cmd.length() > 8)
                player_name = cmd.substr(8);
        }
        else if (cmd.length() > 5)
        {
            player_name = cmd.substr(5);
        }
        std::shared_ptr<STKPeer> player_peer = STKHost::get()->findPeerByName(
            StringUtils::utf8ToWide(player_name));
        if (player_name.empty() || !player_peer || player_peer->isAIPeer())
        {
			std::string msg = "Usage: /kick [player name]";
			sendStringToPeer(msg, peer);
			return;
        }
        else
        {
            if (!isVIP(peer) && !ServerConfig::m_kicks_allowed)
            {
                std::string msg = "Kicking players is not allowed on this server";
                sendStringToPeer(msg, peer);
                return;
            }

			if (!commandPermitted(cmd, peer, hostRights)) return;

            Log::info("ServerLobby", "Player %s kicks %s using /kick", peer_username.c_str(), player_name.c_str());
            player_peer->kick();
            if (ServerConfig::m_track_kicks) {
                std::string auto_report = "[ Auto report caused by kick ]";
                writeOwnReport(player_peer.get(), peer.get(), auto_report);
            }
            if (StringUtils::startsWith(cmd, "kickban"))
            {
                if (isVIP(peer) || (ServerConfig::m_soccer_tournament && hasHostRights(peer)))
                {
                    Log::info("ServerLobby", "%s is now banned", player_name.c_str());
                    m_temp_banned.insert(player_name);
                    std::string msg = StringUtils::insertValues(
                        "%s is now banned", player_name.c_str());
                    sendStringToPeer(msg, peer);
                }
                else
                {
                    std::string msg = "You cannot ban players";
                    sendStringToPeer(msg, peer);
                }
            }
        }
    }
    if (StringUtils::startsWith(cmd, "unban"))
    {
		if (!isVIP(peer) && !(ServerConfig::m_soccer_tournament && hasHostRights(peer)))
        {
			std::string msg = "You cannot unban players";
			sendStringToPeer(msg, peer);
            return;
        }
        std::string player_name;
        if (cmd.length() > 6)
        {
            player_name = cmd.substr(6);
        }
        if (player_name.empty())
        {
			std::string msg = "Usage: /unban [player name]";
			sendStringToPeer(msg, peer);
        }
        else
        {
			if (!commandPermitted(cmd, peer, hostRights)) return;

            Log::info("ServerLobby", "%s is now unbanned", player_name.c_str());
            m_temp_banned.erase(player_name);

			std::string msg = StringUtils::insertValues(
				"%s is now unbanned", player_name.c_str());
			sendStringToPeer(msg, peer);
        }
    }
    if (StringUtils::startsWith(cmd, "ban"))
    {
		if (!isVIP(peer) && !(ServerConfig::m_soccer_tournament && hasHostRights(peer)))
		{
			std::string msg = "You cannot ban players";
			sendStringToPeer(msg, peer);
			return;
		}
        
        std::string player_name;
        if (cmd.length() > 4)
        {
            player_name = cmd.substr(4);
        }
        if (player_name.empty())
        {
            NetworkString* chat = getNetworkString();
            chat->addUInt8(LE_CHAT);
            chat->setSynchronous(true);
            chat->encodeString16(
                L"Usage: /ban [player name]");
            peer->sendPacket(chat, true/*reliable*/);
            delete chat;
        }
        else
        {
			if (!commandPermitted(cmd, peer, hostRights)) return;

            Log::info("ServerLobby", "%s is now banned", player_name.c_str());
            m_temp_banned.insert(player_name);

            std::string msg = StringUtils::insertValues(
                "%s is now banned", player_name.c_str());
            sendStringToPeer(msg, peer);
        }
    }
    if (StringUtils::startsWith(cmd, "playeraddonscore"))
    {
        NetworkString* chat = getNetworkString();
        chat->addUInt8(LE_CHAT);
        chat->setSynchronous(true);
        std::string player_name;
        if (cmd.length() > 17)
            player_name = cmd.substr(17);
        std::shared_ptr<STKPeer> player_peer = STKHost::get()->findPeerByName(
            StringUtils::utf8ToWide(player_name));
        if (player_name.empty() || !player_peer)
        {
            chat->encodeString16(
                L"Usage: /playeraddonscore [player name] (return 0-100)");
        }
        else
        {
            auto& scores = player_peer->getAddonsScores();
            if (scores[AS_KART] == -1 && scores[AS_TRACK] == -1 &&
                scores[AS_ARENA] == -1 && scores[AS_SOCCER] == -1)
            {
                chat->encodeString16(StringUtils::utf8ToWide
                    (player_name + " has no addon"));
            }
            else
            {
                std::string msg = player_name;
                msg += " addon:";
                if (scores[AS_KART] != -1)
                    msg += " kart: " + StringUtils::toString(scores[AS_KART]) + ",";
                if (scores[AS_TRACK] != -1)
                    msg += " track: " + StringUtils::toString(scores[AS_TRACK]) + ",";
                if (scores[AS_ARENA] != -1)
                    msg += " arena: " + StringUtils::toString(scores[AS_ARENA]) + ",";
                if (scores[AS_SOCCER] != -1)
                    msg += " soccer: " + StringUtils::toString(scores[AS_SOCCER]) + ",";
                msg = msg.substr(0, msg.size() - 1);
                chat->encodeString16(StringUtils::utf8ToWide(msg));
            }
        }
        peer->sendPacket(chat, true/*reliable*/);
        delete chat;
    }
    if (argv[0] == "serverhasaddon")
    {
        NetworkString* chat = getNetworkString();
        chat->addUInt8(LE_CHAT);
        chat->setSynchronous(true);
        if (argv.size() != 2)
        {
            chat->encodeString16(
                L"Usage: /serverhasaddon [addon_identity]");
        }
        else
        {
            std::set<std::string> total_addons;
            total_addons.insert(m_addon_kts.first.begin(), m_addon_kts.first.end());
            total_addons.insert(m_addon_kts.second.begin(), m_addon_kts.second.end());
            total_addons.insert(m_addon_arenas.begin(), m_addon_arenas.end());
            total_addons.insert(m_addon_soccers.begin(), m_addon_soccers.end());
            std::string addon_id_test = Addon::createAddonId(argv[1]);
            bool found = total_addons.find(addon_id_test) != total_addons.end();
            if (found)
            {
                chat->encodeString16(StringUtils::utf8ToWide(std::string
                    ("Server has addon ") + argv[1]));
            }
            else
            {
                chat->encodeString16(StringUtils::utf8ToWide(std::string
                    ("Server has no addon ") + argv[1]));
            }
        }
        peer->sendPacket(chat, true/*reliable*/);
        delete chat;
    }
    if (argv[0] == "help")
    {
        NetworkString* chat = getNetworkString();
        chat->addUInt8(LE_CHAT);
        chat->setSynchronous(true);
        chat->encodeString16(m_help_message);
        peer->sendPacket(chat, true/*reliable*/);
        delete chat;
    }
    if (argv[0] == "commands")
    {
        NetworkString* chat = getNetworkString();
        chat->addUInt8(LE_CHAT);
        chat->setSynchronous(true);chat->encodeString16
            (StringUtils::utf8ToWide(m_available_commands));
        peer->sendPacket(chat, true/*reliable*/);
        delete chat;
    }
	if (argv[0] == "gnu2addtrack")
	{
		if (argv.size() > 1)
		{
			std::string newTrack = argv[1];

			if (serverAndPeerHaveTrack(peer, newTrack))
			{
				if (!commandPermitted(cmd, peer, hostRights)) return;

				m_gnu2_available_tracks.insert(m_gnu2_available_tracks.begin(), newTrack);

				NetworkString* chat = getNetworkString();
				chat->addUInt8(LE_CHAT);
				chat->setSynchronous(true);
				std::string message = "Track "+ newTrack +" was added to gnu2 elimination!";
				chat->encodeString16(StringUtils::utf8ToWide(message));
				sendMessageToPeers(chat);
				delete chat;
				return;
			}
			else
			{
				std::string message = "Track " + newTrack + " does not exist or is not installed.";
				sendStringToPeer(message, peer);
				return;
			}
		}
	}
    if (argv[0] == "gnu" || argv[0] == "gnu2")
    {
        if (m_gnu_elimination)
        {
            NetworkString* chat = getNetworkString();
            chat->addUInt8(LE_CHAT);
            chat->setSynchronous(true);
            chat->encodeString16(
                    L"Gnu Elimination mode was already enabled!");
            peer->sendPacket(chat, true/*reliable*/);
            delete chat;
        }
        else if (
            RaceManager::get()->getMinorMode() != RaceManager::MINOR_MODE_NORMAL_RACE &&
            RaceManager::get()->getMinorMode() != RaceManager::MINOR_MODE_TIME_TRIAL)
        {
            NetworkString* chat = getNetworkString();
            chat->addUInt8(LE_CHAT);
            chat->setSynchronous(true);
            chat->encodeString16(
                    L"Gnu Elimination is available only with racing modes");
            peer->sendPacket(chat, true/*reliable*/);
            delete chat;
        }
        else
        {
			if (!commandPermitted(cmd, peer, hostRights)) return;

			if (argv[0] == "gnu2")
			{
				m_gnu2_activated = true;
				m_gnu2_initialized = false;
				m_gnu2_available_tracks.clear();

				std::vector<std::string> gnu2_available_tracks = StringUtils::split(ServerConfig::m_gnu2_available_tracks, ' ');

				for (std::string track : gnu2_available_tracks)
				{
					if (serverAndPeerHaveTrack(peer, track))
						m_gnu2_available_tracks.push_back(track);
				}
			}

            //if (argv.size() > 1 && m_available_kts.first.count(argv[1]) > 0) {
			if (argv.size() > 1 && serverAndPeerHaveKart(peer, argv[1])) {
                m_gnu_kart = argv[1];
            } else {
                m_gnu_kart = "gnu";
            }
            NetworkString* chat = getNetworkString();
            m_gnu_elimination = true;
            m_gnu_remained = -1;
            m_gnu_participants.clear();
            chat->addUInt8(LE_CHAT);
            chat->setSynchronous(true);
            if (m_gnu_kart == "gnu")
            {
                chat->encodeString16(
                    L"Gnu Elimination starts now! Use /standings "
                    "after each race for results.");
            }
            else
            {
                chat->encodeString16(StringUtils::utf8ToWide(
                    StringUtils::insertValues("Gnu Elimination starts now "
                        "(elimination kart: %s)! Use /standings "
                        "after each race for results.", m_gnu_kart)));
            }
            sendMessageToPeers(chat);
            delete chat;
        }
    }
    if (argv[0] == "nognu")
    {        
		if (!m_gnu_elimination)
        {
            NetworkString* chat = getNetworkString();
            chat->addUInt8(LE_CHAT);
            chat->setSynchronous(true);
            chat->encodeString16(
                    L"Gnu Elimination mode was already off!");
            peer->sendPacket(chat, true/*reliable*/);
            delete chat;
        }
        else
        {
			if (!commandPermitted(cmd, peer, hostRights)) return;

            NetworkString* chat = getNetworkString();
            m_gnu_elimination = false;
            m_gnu_remained = 0;
            m_gnu_participants.clear();
			ServerConfig::m_live_players = false;
			m_gnu2_activated = false;
			m_gnu2_initialized = false;
			m_gnu2_available_tracks.clear();
            chat->addUInt8(LE_CHAT);
            chat->setSynchronous(true);
            chat->encodeString16(
                    L"Gnu Elimination is now off");
            sendMessageToPeers(chat);
            delete chat;
        }
    }
    if (argv[0] == "tell")
    {        
        if (argv.size() == 1)
        {
            NetworkString* chat = getNetworkString();
            chat->addUInt8(LE_CHAT);
            chat->setSynchronous(true);
            chat->encodeString16(L"Tell something non-empty");
            peer->sendPacket(chat, true/*reliable*/);
            delete chat;
            return;
        }
        else
        {
            std::string ans;
            for (unsigned i = 1; i < argv.size(); ++i)
            {
                if (i > 1)
                    ans.push_back(' ');
                ans += argv[i];
            }
            writeOwnReport(peer.get(), peer.get(), ans);
        }
    }
    if (argv[0] == "standings")
    {
        if (argv.size() > 1)
        {
            if (argv[1] == "gp")
                sendGrandPrixStandingsToPeer(peer);
            else if (argv[1] == "gnu")
                sendGnuStandingsToPeer(peer);
            else
            {
                std::string msg = "Usage: /standings [gp | gnu]";
                sendStringToPeer(msg, peer);
            }
            return;
        }
        if (m_game_setup->isGrandPrix())
        {
            sendGrandPrixStandingsToPeer(peer);
            return;
        }
        sendGnuStandingsToPeer(peer);
    }
    if (argv[0] == "teamchat")
    {
        NetworkString* chat = getNetworkString();
        chat->addUInt8(LE_CHAT);
        chat->setSynchronous(true);
        m_team_speakers.insert(peer.get());
        chat->encodeString16(L"Your messages are now addressed to team only");        
        peer->sendPacket(chat, true/*reliable*/);
        delete chat;
    }
    if (argv[0] == "to")
    {
        if (argv.size() == 1) {
            NetworkString* chat = getNetworkString();
            chat->addUInt8(LE_CHAT);
            chat->setSynchronous(true);
            chat->encodeString16(L"Usage: /to (username1) ... (usernameN)");
            peer->sendPacket(chat, true/*reliable*/);
            delete chat;
        } else {
            NetworkString* chat = getNetworkString();
            chat->addUInt8(LE_CHAT);
            chat->setSynchronous(true);
            m_message_receivers[peer.get()].clear();
            for (unsigned i = 1; i < argv.size(); ++i) {
                m_message_receivers[peer.get()].insert(
                    StringUtils::utf8ToWide(argv[i]));
            }
            chat->encodeString16(L"Successfully changed chat settings");
            peer->sendPacket(chat, true/*reliable*/);
            delete chat;
        }
    }
    if (argv[0] == "public")
    {
        m_message_receivers[peer.get()].clear();
        m_team_speakers.erase(peer.get());
        std::string s = "Your messages are now public";
        sendStringToPeer(s, peer);
    }
    if (argv[0] == "record")
    {
        NetworkString* chat = getNetworkString();
        chat->addUInt8(LE_CHAT);
        chat->setSynchronous(true);
#ifdef ENABLE_SQLITE3
        if (argv.size() < 5)
        {
            chat->encodeString16(L"Usage: /record (track id) "
                "(normal/time-trial) (normal/reverse) (laps)\n"
                "Receives the server record for the race settings if any");
        } else {
            bool error = false;
            std::string track_name = argv[1];
            std::string mode_name = (argv[2] == "t" || argv[2] == "tt"
                || argv[2] == "time-trial" || argv[2] == "timetrial" ?
                "time-trial" : "normal");
            std::string reverse_name = (argv[3] == "r" ||
                argv[3] == "rev" || argv[3] == "reverse" ? "reverse" : 
                "normal");
            int laps_count = -1;
            if (!StringUtils::parseString<int>(argv[4], &laps_count))
                error = true;
            if (!error && laps_count < 0)
                error = true;
            if (error)
            {
                chat->encodeString16(L"Invalid lap count");
            }
            else
            {
                std::string records_table_name = ServerConfig::m_records_table_name;
                if (!records_table_name.empty())
                {
                    std::string get_query = StringUtils::insertValues("SELECT username, "
                        "result FROM %s LEFT JOIN "
                        "(SELECT venue as v, reverse as r, mode as m, laps as l, "
                        "min(result) as min_res FROM %s group by v, r, m, l) "
                        "ON venue = v and reverse = r and mode = m and laps = l "
                        "WHERE venue = '%s' and reverse = '%s' "
                        "and mode = '%s' and laps = %d and result = min_res;",
                        records_table_name.c_str(), records_table_name.c_str(),
                        track_name.c_str(), reverse_name.c_str(), mode_name.c_str(),
                        laps_count);
                    auto ret = vectorSQLQuery(get_query, 2);
                    if (!ret.first)
                    {
                        chat->encodeString16(L"Failed to make a query");
                    }
                    else if (ret.second[0].size() > 0)
                    {
                        double best_result = 1e18;
                        if (!StringUtils::parseString<double>(
                            ret.second[1][0], &best_result))
                        {
                            chat->encodeString16(L"A strange error occured, "
                                "please take a screenshot "
                                "and contact the server owner.");
                        }
                        else
                        {
                            std::string message = StringUtils::insertValues(
                                "The record is %s by %s",
                                StringUtils::timeToString(best_result),
                                ret.second[0][0]);
                            chat->encodeString16(
                                StringUtils::utf8ToWide(message));
                        }
                    }
                    else
                    {
                        chat->encodeString16(L"No time set yet. Or there is a typo.");
                    }
                }
                else 
                {
                    chat->encodeString16(L"No table storing records!");
                }
            }
        }
#else
        chat->encodeString16(L"This command is not supported.");
#endif
        peer->sendPacket(chat, true/*reliable*/);
        delete chat;
    }
    if (argv[0] == "power")
    {
        if (peer->isAngryHost())
        {
            peer->setAngryHost(false);
            std::string msg = "You are now a normal player";
            sendStringToPeer(msg, peer);
            updatePlayerList();
            return;
        }
        std::string password = ServerConfig::m_power_password;
        if (password.empty() || argv.size() <= 1 || argv[1] != password)
        {
            std::string msg = "You need to provide the password to have the power";
            sendStringToPeer(msg, peer);
            return;
        }
        peer->setAngryHost(true);
        std::string msg = "Now you finally have the power!";
        sendStringToPeer(msg, peer);
        updatePlayerList();
        return;
    }
    else if (argv[0] == "admin")
    {
        std::string msg;
        if (!peer->isAngryHost() && !ServerConfig::m_soccer_tournament) {
            msg = "You cannot control this server";
            sendStringToPeer(msg, peer);
            return;
        }
        if (argv.size() == 1) {
            msg = "Usage: /admin command arg1 arg2 ...";
            sendStringToPeer(msg, peer);
            return;
        }
        if (argv[1] == "start") {
            if (argv.size() == 2 || !(argv[2] == "0" || argv[2] == "1")) {
                msg = "Usage: /admin start [0/1] - allow or forbid starting a race";
                sendStringToPeer(msg, peer);
                return;
            }
            if (argv[2] == "0") {
                m_allowed_to_start = false;
                msg = "Now starting a race is forbidden";
            } else {
                m_allowed_to_start = true;
                msg = "Now starting a race is allowed";
            }
            sendStringToPeer(msg, peer);
            return;
        }
    }
    if (argv[0] == "version")
    {
        std::string msg = "1.2-rc1-kimden 200824 including Rocker/Waldlaubsaengernest changes";
        sendStringToPeer(msg, peer);
    }
#ifdef ENABLE_WEB_SUPPORT
    if (argv[0] == "token")
    {
        int online_id = peer->getPlayerProfiles()[0]->getOnlineId();
        if (online_id <= 0)
        {
            std::string msg = "Please join with a valid online STK account.";
            sendStringToPeer(msg, peer);
            return;
        }
        std::string username = StringUtils::wideToUtf8(
            peer->getPlayerProfiles()[0]->getName());
        std::string token = getToken();
        while (m_web_tokens.count(token))
            token = getToken();
        m_web_tokens.insert(token);
        std::string msg = "Your token is " + token;
#ifdef ENABLE_SQLITE3
        std::string tokens_table_name = ServerConfig::m_tokens_table;
        std::string query = StringUtils::insertValues(
            "INSERT INTO %s (username, token) "
            "VALUES (\"%s\", \"%s\");",
            tokens_table_name.c_str(), username.c_str(), token.c_str()
        );
        if (easySQLQuery(query))
            msg += "\nRetype it on the website to connect your STK account. ";
        else
            msg = "An error occurred, please try again.";
#else
        msg += "\nThough it is useless...";
#endif
        sendStringToPeer(msg, peer);
    }
#endif
	if (argv[0] == "setfield" || argv[0] == "settrack")
	{
		bool isField = (argv[0] == "setfield");

		if (argv.size() != 2)
		{
			std::string msg = isField ? "Format: /setfield soccer_field_id" : "Format: /settrack track_id";
			sendStringToPeer(msg, peer);
			return;
		}

		std::string peer_username = StringUtils::wideToUtf8(peer->getPlayerProfiles()[0]->getName());

		std::string soccer_field_id = argv[1];

		if (soccer_field_id == "ice") soccer_field_id = "icy_soccer_field";
		else if (soccer_field_id == "grass") soccer_field_id = "soccer_field";
		else if (soccer_field_id == "lasdunas") soccer_field_id = "lasdunassoccer";
		else if (soccer_field_id == "egypt") soccer_field_id = "addon_egypt_1";
		else if (soccer_field_id == "tourn") soccer_field_id = "addon_tournament-field";
		else if (soccer_field_id == "zen") soccer_field_id = "addon_zen";
		else if (soccer_field_id == "cosmic") soccer_field_id = "addon_cosmic";
		else if (soccer_field_id == "holedrop") soccer_field_id = "addon_hole-drop";
		else if (soccer_field_id == "forest") soccer_field_id = "addon_forest_1";
		else if (soccer_field_id == "another") soccer_field_id = "addon_another-soccer-field";
		else if (soccer_field_id == "airhockey") soccer_field_id = "addon_air-hockey";
		else if (soccer_field_id == "database") soccer_field_id = "addon_database";
		else if (soccer_field_id == "math" || soccer_field_id == "pidgin") soccer_field_id = "addon_math-class";
		else if (soccer_field_id == "ex1") soccer_field_id = "addon_experimental-plane---field-1";
		else if (soccer_field_id == "ex2") soccer_field_id = "addon_experimental-plane---field-2";
		else if (soccer_field_id == "ex3") soccer_field_id = "addon_experimental-plane---field-3";
		else if (soccer_field_id == "inapit" || soccer_field_id == "roml") soccer_field_id = "addon_inapit";
		else if (soccer_field_id == "nitro") soccer_field_id = "addon_nitro-soccer-field";
		else if (soccer_field_id == "vacuum") soccer_field_id = "addon_vivid-vacuum";
		else if (soccer_field_id == "mountain") soccer_field_id = "addon_mountain-soccer--updated-";
		else if (soccer_field_id == "box") soccer_field_id = "addon_box";
		else if (soccer_field_id == "soccerarena") soccer_field_id = "addon_soccer-arena-x";
		else if (soccer_field_id == "super") soccer_field_id = "addon_supertournament-field";
		else if (soccer_field_id == "asteroid") soccer_field_id = "addon_asteroid-soccer";

		else if (soccer_field_id == "myoldtrack") soccer_field_id = "addon_myoldtrack";
		else if (soccer_field_id == "xtreme" || soccer_field_id == "xtremetrack") soccer_field_id = "addon_x-treme-track";
		else if (soccer_field_id == "mini") soccer_field_id = "addon_minigolf";
		else if (soccer_field_id == "animtrack") soccer_field_id = "addon_animtrack_1";
		else if (soccer_field_id == "aroundthebox") soccer_field_id = "addon_around-the-box_2";
		else if (soccer_field_id == "bowling") soccer_field_id = "addon_bowling";
		else if (soccer_field_id == "gravity") soccer_field_id = "addon_gravitytrack";
		else if (soccer_field_id == "wrecktrack") soccer_field_id = "addon_wrecktrack";
		else if (soccer_field_id == "escape" || soccer_field_id == "escaperoom") soccer_field_id = "addon_escape-room";
		else if (soccer_field_id == "escape-multi") soccer_field_id = "addon_escape-room-mp";
		else if (soccer_field_id == "teamwork") soccer_field_id = "addon_teamwork_1";
		else if (soccer_field_id == "jumptrack") soccer_field_id = "addon_jumptrack";


		// Check that peer and server have the track
		std::shared_ptr<STKPeer> player_peer = STKHost::get()->findPeerByName(StringUtils::utf8ToWide(peer_username));

		bool found = serverAndPeerHaveTrack(player_peer, soccer_field_id) || soccer_field_id == "all";

		if (!(found))
		{
			std::string addon_id = "addon_" + soccer_field_id;
			bool found_addon = serverAndPeerHaveTrack(player_peer, addon_id);
			if (found_addon)
			{
				soccer_field_id = addon_id;
				found = true;
			}
		}

		if (found)
		{
			if (!commandPermitted(cmd, peer, hostRights)) return;

			if (soccer_field_id == "all")
			{
				m_set_field = "";
				std::string msg = isField ? "All soccer fields can be played again" : "All tracks can be played again";
				sendStringToPeer(msg, peer);
				Log::info("ServerLobby", "setfield all");
				return;
			}
			else
			{
				m_set_field = soccer_field_id;

				std::string msg = isField ? "Next played soccer field will be " + soccer_field_id + "." :
					"Next played track will be " + soccer_field_id + ".";

				// Send message to the lobby
				sendStringToAllPeers(msg);

				std::string msg2 = "setfield " + soccer_field_id;
				Log::info("ServerLobby", msg2.c_str());
			}
		}
		else
		{
			std::string msg = isField ? "Soccer field \'" + soccer_field_id + "\' does not exist or is not installed." :
				"Track \'" + soccer_field_id + "\' does not exist or is not installed.";
			sendStringToPeer(msg, peer);
			return;
		}
	}
	
	if (argv[0] == "setkart")
	{
		if (argv.size() != 2 && argv.size() != 3)
		{
			std::string msg = "Format: /setkart kart_name [player_name]";
			sendStringToPeer(msg, peer);
			return;
		}

		std::string peer_username = StringUtils::wideToUtf8(peer->getPlayerProfiles()[0]->getName());

		std::string kart_name = argv[1];
		std::string user_name = (argv.size() == 3 ? argv[2] : peer_username);

		bool serverHasKart = (m_official_kts.first.find(kart_name) != m_official_kts.first.end()) ||
			(m_addon_kts.first.find(kart_name) != m_addon_kts.first.end());

		if (!serverHasKart)
		{
			std::string addon_kart_name = "addon_" + kart_name;
			bool serverHasAddonKart = (m_official_kts.first.find(addon_kart_name) != m_official_kts.first.end()) ||
				(m_addon_kts.first.find(addon_kart_name) != m_addon_kts.first.end());

			if (serverHasAddonKart)
			{
				serverHasKart = true;
				kart_name = addon_kart_name;
			}
		}

		if (serverHasKart || kart_name == "all")
		{
			if (!commandPermitted(cmd, peer, hostRights)) return;

			if (kart_name == "all")
			{
				m_set_field = "";
				if (m_set_kart.count(user_name))
					m_set_kart.erase(user_name);
				std::string msg = user_name + " can use all karts again.";
				sendStringToAllPeers(msg);
				Log::info("ServerLobby", "setkart all");
				return;
			}
			else
			{
				m_set_kart[user_name] = kart_name;
				std::string msg = user_name + " will play with " + kart_name + ".";

				// Send message to the lobby
				sendStringToAllPeers(msg);

				std::string msg2 = "setkart " + kart_name;
				Log::info("ServerLobby", msg2.c_str());
			}
		}
		else
		{
			std::string msg = "Kart \'" + kart_name + "\' does not exist or is not installed.";
			sendStringToPeer(msg, peer);
			return;
		}

	}

	if (argv[0] == "sethost")
	{
		if (argv.size() != 1 && argv.size() != 2)
		{
			std::string msg = "Format: /sethost [player_name]";
			sendStringToPeer(msg, peer);
			return;
		}

		std::string peer_username = StringUtils::wideToUtf8(peer->getPlayerProfiles()[0]->getName());
		std::string user_name = (argv.size() == 2 ? argv[1] : peer_username);
		if (argv.size() == 1)
			cmd += " " + user_name;

		std::shared_ptr<STKPeer> player_peer = STKHost::get()->findPeerByName(StringUtils::utf8ToWide(user_name));

		if (player_peer)
		{
			if (!commandPermitted(cmd, peer, hostRights)) return;

			// updateServerOwner()
			NetworkString* ns = getNetworkString();
			ns->setSynchronous(true);
			ns->addUInt8(LE_SERVER_OWNERSHIP);
			player_peer->sendPacket(ns);
			delete ns;
			m_server_owner = player_peer;
			m_server_owner_id.store(player_peer->getHostId());
			updatePlayerList();
			
			std::string msg = "New server host is " + user_name;
			sendStringToAllPeers(msg);

			std::string msg2 = "sethost " + user_name;
			Log::info("ServerLobby", msg2.c_str());
		}
		else
		{
			std::string msg = "Player " + user_name + " is not in the lobby.";
			sendStringToPeer(msg, peer);
			return;
		}
	}
	
	if (argv[0] == "mode")
	{
		if (argv.size() != 2)
		{
			std::string msg = "Format: /mode {grand-prix-normal, grand-prix-time, normal, time, soccer-time, soccer-goal, free-for-all, capture-the-flag}";
			sendStringToPeer(msg, peer);
			return;
		}
		else
		{
			unsigned char difficulty = m_difficulty.load();
			unsigned char gameMode = 0;
			unsigned char soccerGoalTarget = 0;
			bool serverModeValid = stringToServerMode(argv[1], gameMode, soccerGoalTarget);

			if (serverModeValid)
			{
				if (m_available_modes.count(gameMode) == 0)
				{
					std::string msg = "Mode \"" + serverModeToString(gameMode, soccerGoalTarget) + "\" is not available on this server.";
					sendStringToPeer(msg, peer);
					return;
				}

				if (!commandPermitted(cmd, peer, hostRights)) return;

				setServerMode(difficulty, gameMode, soccerGoalTarget, peer);
			}
			else
			{
				std::string msg = "Mode \"" + argv[1] + "\" does not exist.";
				sendStringToPeer(msg, peer);
				return;
			}
		}
	}
	if (ServerConfig::m_super_tournament)
        {
        std::string peer_username = StringUtils::wideToUtf8(peer->getPlayerProfiles()[0]->getName());
        if (argv[0] == "join")
        {
            std::string kali="python3 join.py " +peer_username;
            system(kali.c_str());
            std::string msg = "Successfully joined the tournament.";
            sendStringToPeer(msg, peer);
        }
        if (argv[0] == "ican" || argv[0] == "icant")
        {
            if (argv.size() == 1)
            {
                std::string msg = "None good - u wrong use command";
                sendStringToPeer(msg, peer);
		return;
            }
            bool valid_time=(argv[1]=="mo16" || argv[1]=="mo17" || argv[1]=="mo18" || argv[1]=="mo19" || argv[1]=="tu16" || argv[1]=="tu17" || argv[1]=="tu18" || argv[1]=="tu19" || argv[1]=="we16" || argv[1]=="we17" || argv[1]=="we18" || argv[1]=="we19" || argv[1]=="th16" || argv[1]=="th17" || argv[1]=="th18" || argv[1]=="th19" ||argv[1]=="fr16" || argv[1]=="fr17" || argv[1]=="fr18" || argv[1]=="fr19" ||argv[1]=="sa16" || argv[1]=="sa17" || argv[1]=="sa18" || argv[1]=="sa19" || argv[1]=="su16" || argv[1]=="su17" || argv[1]=="su18" || argv[1]=="su19" || argv[1]=="mo" || argv[1]=="tu" || argv[1]=="we" || argv[1]=="th" || argv[1]=="fr" || argv[1]=="sa" ||argv[1]=="su" || argv[1]=="weekdays" || argv[1]=="weekends" || argv[1]=="weekdays16" || argv[1]=="weekends16" ||argv[1]=="weekdays17" || argv[1]=="weekends17" ||argv[1]=="weekdays18" || argv[1]=="weekends18" || argv[1]=="weekdays19" || argv[1]=="weekends19" ||argv[1]=="16" || argv[1]=="17" || argv[1]=="18" || argv[1]=="19"||argv[1]=="all");
            if(valid_time)
            {
                std::string kali="python3 time_poll.py " +peer_username+" "+argv[1]+" "+argv[0];
                system(kali.c_str());
                std::string msg = "Successfully edited timepoll.";
                sendStringToPeer(msg, peer);
            }
            else
            {
                std::string msg = "Please specify a valid time. Format: /ican mo16 (meaning I can Monday 16 UTC), /ican tu (I can on Tuesdays), /ican weekdays (I can on weekdays), /ican weekends17 (I can on weekends at 17 UTC), /ican 18 (I can each day on 18 UTC), /all (I can at every time). Same format for /icant. Note that /icant only has an effect after using /ican at least once.";
                sendStringToPeer(msg, peer);
            }
        }
        if (argv[0] == "count")
        {
	    if (m_tournament_referees.count(peer_username) == 0 && !(isVIP(peer)))
            {
                std::string msg = "You are not a referee";
                sendStringToPeer(msg, peer);
                return;
            }

            ServerConfig::m_count_supertournament_game=true;
            std::string msg = "Counting enabled.";
            sendStringToPeer(msg, peer);
        }
        if (argv[0] == "nocount")
        {
	    if (m_tournament_referees.count(peer_username) == 0 && !(isVIP(peer)))
            {
                std::string msg = "You are not a referee";
                sendStringToPeer(msg, peer);
                return;
            } 
            ServerConfig::m_count_supertournament_game=false;
            std::string msg = "Counting disabled.";
            sendStringToPeer(msg, peer);
        }
        if (argv[0] == "setteams")
        {
	    if (m_tournament_referees.count(peer_username) == 0 && !(isVIP(peer)))
            {
                std::string msg = "You are not a referee";
                sendStringToPeer(msg, peer);
                return;
            }
            if (argv[1]=="A" || argv[1]=="B" || argv[1]=="C" || argv[1]=="D")
            {
                if (argv[2]=="A" || argv[2]=="B" || argv[2]=="C" || argv[2]=="D")
		{
                    ServerConfig::m_red_team_name=argv[1];
                    ServerConfig::m_blue_team_name=argv[2];
                    std::string msg = "Next match will be "+argv[1]+" vs "+argv[2]+".";
                    sendStringToPeer(msg, peer);
		}
	    }
	    else
	    {
                std::string msg = "Please use A, B, C or D as team name.";                                                                 
                sendStringToPeer(msg, peer); 
	    }
        }
        if (argv[0] == "yellow")
        {
	    if (m_tournament_referees.count(peer_username) == 0 && !(isVIP(peer)))
            {
                std::string msg = "You are not a referee";
                sendStringToPeer(msg, peer);
                return;
            }
            int len_argv = argv.size();
            int v1=2;
            std::string msg = argv[1]+" was shown a yellow card by the Referee. Reason:";
            printf("%i",len_argv);
            while (v1<len_argv)
            {
                msg+=(" "+argv[v1]);
                v1++;
            }
            sendStringToAllPeers(msg);
            std::string ringdrossel="python3 supertournament_yellow.py "+argv[1];
            system(ringdrossel.c_str());
        }
	if (argv[0] == "addon")
        {
            if (m_tournament_referees.count(peer_username) == 0 && !(isVIP(peer)))
            {
                std::string msg = "You are not a referee";
                sendStringToPeer(msg, peer);
                return;
            }
            std::string blau=ServerConfig::m_blue_team_name;
            std::string rot=ServerConfig::m_red_team_name;
            std::string ringdrossel="python3 supertournament_match_info.py "+argv[1]+" Addon "+rot+" "+blau;
            system(ringdrossel.c_str());
            std::string msg = "Succesfully edited Addon.";
            sendStringToPeer(msg, peer);
        }
	if (argv[0] == "server")
        {
            if (m_tournament_referees.count(peer_username) == 0 && !(isVIP(peer)))
            {
                std::string msg = "You are not a referee";
                sendStringToPeer(msg, peer);
                return;
            }
            std::string blau=ServerConfig::m_blue_team_name;
            std::string rot=ServerConfig::m_red_team_name;
            std::string ringdrossel="python3 supertournament_match_info.py "+argv[1]+" Server "+rot+" "+blau;
            system(ringdrossel.c_str());
            std::string msg = "Succesfully edited Server.";
            sendStringToPeer(msg, peer);
        }
	if (argv[0] == "referee")
        {
            if (m_tournament_referees.count(peer_username) == 0 && !(isVIP(peer)))
            {
                std::string msg = "You are not a referee";
                sendStringToPeer(msg, peer);
                return;
            }
            std::string blau=ServerConfig::m_blue_team_name;
            std::string rot=ServerConfig::m_red_team_name;
            std::string ringdrossel="python3 supertournament_match_info.py "+argv[1]+" Referee "+rot+" "+blau;
            system(ringdrossel.c_str());
            std::string msg = "Succesfully edited Referee.";
            sendStringToPeer(msg, peer);
        }
	if (argv[0] == "video")
        {
            if (m_tournament_referees.count(peer_username) == 0 && !(isVIP(peer)))
            {
                std::string msg = "You are not a referee";
                sendStringToPeer(msg, peer);
                return;
            }
            std::string blau=ServerConfig::m_blue_team_name;
            std::string rot=ServerConfig::m_red_team_name;
            std::string ringdrossel="python3 supertournament_match_info.py "+argv[1]+" Video "+rot+" "+blau;
            system(ringdrossel.c_str());
            std::string msg = "Succesfully edited video link.";
            sendStringToPeer(msg, peer);
        }
	if (argv[0] == "notes")
        {
            if (m_tournament_referees.count(peer_username) == 0 && !(isVIP(peer)))
            {
                std::string msg = "You are not a referee";
                sendStringToPeer(msg, peer);
                return;
            }
            std::string blau=ServerConfig::m_blue_team_name;
            std::string rot=ServerConfig::m_red_team_name;
            std::string ringdrossel="python3 supertournament_match_info.py "+argv[1]+" Notes "+rot+" "+blau;
            system(ringdrossel.c_str());
            std::string msg = "Succesfully edited notes.";
            sendStringToPeer(msg, peer);
        }
	if (argv[0] == "skip")
        {
            if (m_tournament_referees.count(peer_username) == 0 && !(isVIP(peer)))
            {
                std::string msg = "You are not a referee";
                sendStringToPeer(msg, peer);
                return;
            }
	    ServerConfig::m_skip_end=true;
            std::string msg = "Skipping end enabled.";
            sendStringToPeer(msg, peer);
        }
	if (argv[0] == "noskip")
        {
            if (m_tournament_referees.count(peer_username) == 0 && !(isVIP(peer)))
            {
                std::string msg = "You are not a referee";
                sendStringToPeer(msg, peer);
                return;
            }
            ServerConfig::m_skip_end=false;
            std::string msg = "Skipping end disabled.";
            sendStringToPeer(msg, peer);
        }
    }
    if (ServerConfig::m_soccer_tournament)
    {
        std::string peer_username = StringUtils::wideToUtf8(
            peer->getPlayerProfiles()[0]->getName());
        if (m_tournament_referees.count(peer_username) == 0 && !(isVIP(peer))) 
	    {
            if(!ServerConfig::m_super_tournament)
	        {
		        std::string msg = "You are not a referee";
                sendStringToPeer(msg, peer);
	        }
            return;
        }
        if (argv[0] == "game")
        {
	    m_tournament_max_games=6;
            int old_game = m_tournament_game;
            if (argv.size() < 2) {
                ++m_tournament_game;
                if (m_tournament_game == m_tournament_max_games)
                    m_tournament_game = 0;
                m_fixed_lap = 10;
            } else {
                if (!StringUtils::parseString(argv[1], &m_tournament_game)
                    || m_tournament_game < 0
                    || m_tournament_game >= m_tournament_max_games)
                { 
                    std::string msg = "Please specify a correct number. "
                        "Format: /game [number 0.."
                        + std::to_string(m_tournament_max_games - 1) + "] [length]";
                    sendStringToPeer(msg, peer);
                    return;
                }
                int length = 7;
                if (argv.size() >= 3)
                {
                    bool ok = StringUtils::parseString(argv[2], &length);
                    if (!ok || length <= 0)
                    {
                        std::string msg = "Please specify a correct number. "
                            "Format: /game [number] [length]";
                        sendStringToPeer(msg, peer);
                        return;
                    }
                }
                m_fixed_lap = length;
            }
            //if (tournamentColorsSwapped(m_tournament_game) ^ tournamentColorsSwapped(old_game))
            //    changeColors();
            if (tournamentGoalsLimit(m_tournament_game) ^ tournamentGoalsLimit(old_game))
                changeLimitForTournament(tournamentGoalsLimit(m_tournament_game));
            std::string msg = StringUtils::insertValues(
                "Ready to start game %d for %d ", m_tournament_game, m_fixed_lap)
                + (tournamentGoalsLimit(m_tournament_game) ? "goals" : "minutes");
            sendStringToAllPeers(msg);
        }
        else if (argv[0] == "role")
        {
            if (argv.size() < 3)
            {
                std::string msg = "Format: /role (R|B|J|S) username";
                sendStringToPeer(msg, peer);
                return;
            }
            std::string role = argv[1];
            std::string username = argv[2];
            bool permanent = (argv.size() >= 4 &&
                (argv[3] == "p" || argv[3] == "permanent"));
            if (role.length() != 1)
                std::swap(role, username);
            if (role.length() != 1)
            {
                std::string msg = "Please specify one-letter role (R/B/J/S) and player";
                sendStringToPeer(msg, peer);
                return;
            }
            m_tournament_red_players.erase(username);
            m_tournament_blue_players.erase(username);
            m_tournament_referees.erase(username);
            if (permanent)
            {
                m_tournament_init_red.erase(username);
                m_tournament_init_blue.erase(username);
                m_tournament_init_ref.erase(username);
            }
            std::string role_changed = "The referee has updated your role - you are now %s";
            std::shared_ptr<STKPeer> player_peer = STKHost::get()->findPeerByName(
                StringUtils::utf8ToWide(username));
            switch (role[0])
            {
                case 'R':
                case 'r':
                {
                    m_tournament_red_players.insert(username);
                    if (player_peer)
                    {
                        role_changed = StringUtils::insertValues(role_changed, "red player");
                        player_peer->getPlayerProfiles()[0]->setTeam(KART_TEAM_RED);
                        player_peer->setAlwaysSpectate(false);
                        sendStringToPeer(role_changed, player_peer);
                    }
                    break;
                }
                case 'B':
                case 'b':
                {
                    m_tournament_blue_players.insert(username);
                    if (player_peer)
                    {
                        role_changed = StringUtils::insertValues(role_changed, "blue player");
                        player_peer->getPlayerProfiles()[0]->setTeam(KART_TEAM_BLUE);
                        player_peer->setAlwaysSpectate(false);
                        sendStringToPeer(role_changed, player_peer);
                    }
                    break;
                }
                case 'J':
                case 'j':
                {
                    m_tournament_referees.insert(username);
                    if (permanent)
                        m_tournament_init_ref.insert(username);
                    if (player_peer)
                    {
                        role_changed = StringUtils::insertValues(role_changed, "referee");
                        player_peer->getPlayerProfiles()[0]->setTeam(KART_TEAM_NONE);
                        sendStringToPeer(role_changed, player_peer);
                    }
                    break;
                }
                case 'S':
                case 's':
                {
                    if (player_peer)
                    {
                        role_changed = StringUtils::insertValues(role_changed, "spectator");
                        player_peer->getPlayerProfiles()[0]->setTeam(KART_TEAM_NONE);
                        sendStringToPeer(role_changed, player_peer);
                    }
                    break;
                }
            }
            std::string msg = StringUtils::insertValues(
                "Successfully changed role to %s for %s", role, username);
            sendStringToPeer(msg, peer);
            updatePlayerList();
        }
	else if (argv[0] == "stop")
        {
            World* w = World::getWorld();
            if (!w)
                return;
            SoccerWorld *sw = dynamic_cast<SoccerWorld*>(w);
            sw->stop();
            std::string msg = "The game is stopped.";
            sendStringToAllPeers(msg);
        }
        else if (argv[0] == "go" || argv[0] == "play" || argv[0] == "resume")
        {
            World* w = World::getWorld();
            if (!w)
                return;
            SoccerWorld *sw = dynamic_cast<SoccerWorld*>(w);
            sw->resume();
            std::string msg = "The game is resumed.";
            sendStringToAllPeers(msg);
        }
        else if (argv[0] == "lobby")
        {
            World* w = World::getWorld();
            if (!w)
                return;
            SoccerWorld *sw = dynamic_cast<SoccerWorld*>(w);
            sw->allToLobby();
            std::string msg = "The game will be restarted or continued.";
            sendStringToAllPeers(msg);
        }
        else if (argv[0] == "init")
        {
            int red, blue;
            if (argv.size() < 3 ||
                !StringUtils::parseString<int>(argv[1], &red) ||
                !StringUtils::parseString<int>(argv[2], &blue))
            {
                std::string msg = "Usage: /init [red_count] [blue_count]";
                sendStringToPeer(msg, peer);
                return;
            }
            World* w = World::getWorld();
            if (!w)
            {
                std::string msg = "Please set the count when the karts "
                    "are ready. Setting the initial count in lobby is "
                    "not implemented yet, sorry.";
                sendStringToPeer(msg, peer);
                return;
            }
            SoccerWorld *sw = dynamic_cast<SoccerWorld*>(w);
            sw->setInitialCount(red, blue);
            sw->tellCount();
        }
    }
	if (ServerConfig::m_race_tournament)
	{
		std::string peer_username = StringUtils::wideToUtf8(
            peer->getPlayerProfiles()[0]->getName());
        if (m_race_tournament_referees.count(peer_username) == 0 && !(isVIP(peer))) 
		{
            std::string msg = "You are not a referee";
            sendStringToPeer(msg, peer);
            return;
        }
        if (argv[0] == "game")
        {
            int old_game = m_tournament_game;
            if (argv.size() < 2) {
                ++m_tournament_game;
                m_fixed_lap = 3;
            } else {
                if (!StringUtils::parseString(argv[1], &m_tournament_game))
                { 
                    std::string msg = "Please specify a correct number. "
                        "Format: /game [number] [length]";
                    sendStringToPeer(msg, peer);
                    return;
                }
                int length = 3;
                if (argv.size() >= 3)
                {
                    bool ok = StringUtils::parseString(argv[2], &length);
                    if (!ok || length <= 0)
                    {
                        std::string msg = "Please specify a correct number. "
                            "Format: /game [number] [length]";
                        sendStringToPeer(msg, peer);
                        return;
                    }
                }
                m_fixed_lap = length;
            }
            std::string msg = StringUtils::insertValues(
                "Ready to start game %d for %d laps.", m_tournament_game, m_fixed_lap);
            sendStringToAllPeers(msg);
        }
        else if (argv[0] == "role")
        {
            if (argv.size() < 3)
            {
                std::string msg = "Format: /role (J|P|S) username";
                sendStringToPeer(msg, peer);
                return;
            }
            std::string role = argv[1];
            std::string username = argv[2];
            if (role.length() != 1)
                std::swap(role, username);
            if (role.length() != 1)
            {
                std::string msg = "Please specify one-letter role (J/P/S) and player";
                sendStringToPeer(msg, peer);
                return;
            }
            m_race_tournament_players.erase(username);
            m_race_tournament_referees.erase(username);
            std::string role_changed = "The referee has updated your role - you are now %s";
            std::shared_ptr<STKPeer> player_peer = STKHost::get()->findPeerByName(
                StringUtils::utf8ToWide(username));
            switch (role[0])
            {
                case 'P':
                case 'p':
                {
					m_race_tournament_players.insert(username);
                    if (player_peer)
                    {
                        role_changed = StringUtils::insertValues(role_changed, "player");
						player_peer->setAlwaysSpectate(false);
                        sendStringToPeer(role_changed, player_peer);
                    }
                    break;
                }
                case 'J':
                case 'j':
                {
                    m_tournament_referees.insert(username);
                    if (player_peer)
                    {
                        role_changed = StringUtils::insertValues(role_changed, "referee");
						player_peer->setAlwaysSpectate(true);
                        sendStringToPeer(role_changed, player_peer);
                    }
                    break;
                }
                case 'S':
                case 's':
                {
                    if (player_peer)
                    {
                        role_changed = StringUtils::insertValues(role_changed, "spectator");
						player_peer->setAlwaysSpectate(true);
                        sendStringToPeer(role_changed, player_peer);
                    }
                    break;
                }
            }
            std::string msg = StringUtils::insertValues(
                "Successfully changed role to %s for %s", role, username);
            sendStringToPeer(msg, peer);
            updatePlayerList();
        }
	}
}   // handleServerCommand
//-----------------------------------------------------------------------------
void ServerLobby::updateGnuElimination()
{
    World* w = World::getWorld();
    assert(w);
    assert(m_gnu_remained != 0);
    int player_count = RaceManager::get()->getNumPlayers();
    const double INF = 1e9;
    std::vector<std::pair<double, std::string>> order;
    if (m_gnu_remained < 0)
    {
        for (int i = 0; i < player_count; i++)
        {
            std::string username = StringUtils::wideToUtf8(RaceManager::get()->getKartInfo(i).getPlayerName());
            double elapsed_time = (w->getKart(i)->isEliminated() ? INF :
                RaceManager::get()->getKartRaceTime(i));
            order.emplace_back(elapsed_time, username);
            m_gnu_participants.push_back(username);
        }
        m_gnu_remained = player_count;
    }
    else
    {
        for (unsigned i = 0; i < m_gnu_participants.size(); i++)
        {
            order.emplace_back(INF, m_gnu_participants[i]);
        }
        // the number of players is very small and I don't want maps
        for (int i = 0; i < player_count; i++)
        {
            std::string username = StringUtils::wideToUtf8(RaceManager::get()->getKartInfo(i).getPlayerName());
            double elapsed_time = (w->getKart(i)->isEliminated() ? INF :
                RaceManager::get()->getKartRaceTime(i));
            for (int j = 0; j < m_gnu_remained; j++)
            {
                if (m_gnu_participants[j] == username)
                {
                    order[j].first = elapsed_time;
                    break;
                }
            }
        }
    }

    std::stable_sort(order.begin(), order.begin() + m_gnu_remained);
    bool all_quit = false;//order[0].first == INF;
    for (int i = 0; i < m_gnu_remained; i++)
        m_gnu_participants[i] = order[i].second;
    --m_gnu_remained;
    if (!all_quit)
    {
        while (m_gnu_remained - 1 >= 0 && order[m_gnu_remained - 1].first == INF)
            --m_gnu_remained;
    }

    if (m_gnu_remained <= 1) {
        m_gnu_elimination = false;
		m_gnu2_activated = false;
		m_gnu2_initialized = false;
		m_gnu2_available_tracks.clear();
        NetworkString* chat = getNetworkString();
        chat->addUInt8(LE_CHAT);
        chat->setSynchronous(true);
        std::string message = "Gnu Elimination has finished! Congratulations to " + m_gnu_participants[0] + " !";
        chat->encodeString16(StringUtils::utf8ToWide(message));
        sendMessageToPeers(chat);
        delete chat;
    }

	if (m_gnu_elimination && m_gnu2_activated)
	{
		// Remove the played track from the list
		std::string played_track = RaceManager::get()->getTrackName();
		int remove_track_idx = -1;
		for (int i = 0; i < m_gnu2_available_tracks.size(); i++)
			if (m_gnu2_available_tracks[i] == played_track)
				remove_track_idx = i;

		if (remove_track_idx != -1)
			m_gnu2_available_tracks.erase(m_gnu2_available_tracks.begin() + remove_track_idx);

		// Ensure, that the number of available tracks matches the number of remaining players
		int tracks_count = m_gnu_remained - 1;
		if (tracks_count > 0)
		{
			if (m_gnu2_available_tracks.size() > tracks_count)
				m_gnu2_available_tracks.erase(m_gnu2_available_tracks.begin() + tracks_count, m_gnu2_available_tracks.end());
			else if (m_gnu2_available_tracks.size() < tracks_count)
				selectRandomTracks(m_gnu2_available_tracks, tracks_count);
		}
	}
}  // updateGnuElimination
//-----------------------------------------------------------------------------
void ServerLobby::selectRandomTracks(std::vector<std::string> &tracks, int newTrackCount)
{
	std::vector<std::string> availableTracks;
	for (std::string track : m_available_kts.second)
		if (std::find(tracks.begin(), tracks.end(), track) == tracks.end())
			availableTracks.push_back(track);

	selectRandomTracks(tracks, availableTracks, newTrackCount);
}  // selectRandomTracks

void ServerLobby::selectRandomTracks(std::vector<std::string> &tracks, std::vector<std::string> &availableTracks, int newTrackCount)
{
	if (newTrackCount < 0) return; // Avoid an infinite loop

	std::srand(std::time(nullptr));
	if (tracks.size() < newTrackCount)
	{
		if (tracks.size() == 0 && availableTracks.size() == 0) // Avoid an infinite loop if no tracks are available
			return;

		while (tracks.size() < newTrackCount && availableTracks.size() > 0)
		{
			int randomIndex = std::rand() % availableTracks.size();
			tracks.push_back(availableTracks[randomIndex]);
			availableTracks.erase(availableTracks.begin() + randomIndex);
		}

		if (tracks.size() < newTrackCount)
		{
			int tracksSize = tracks.size();
			int i = 0;
			while (tracks.size() < newTrackCount)
			{
				tracks.push_back(tracks[i % tracksSize]);
				i++;
			}
		}
	}
	else
	{
		while (tracks.size() > newTrackCount)
		{
			int randomIndex = std::rand() % tracks.size();
			tracks.erase(tracks.begin() + randomIndex);
		}
	}
}  // selectRandomTracks
//-----------------------------------------------------------------------------
bool ServerLobby::serverAndPeerHaveTrack(std::shared_ptr<STKPeer>& peer, std::string track_id) const
{
	return serverAndPeerHaveTrack(peer.get(), track_id);
}

bool ServerLobby::serverAndPeerHaveTrack(STKPeer* peer, std::string track_id) const
{
	std::pair<std::set<std::string>, std::set<std::string>> kt = peer->getClientAssets();
	bool peerHasTrack = kt.second.find(track_id) != kt.second.end();

	bool serverHasTrack = (m_official_kts.second.find(track_id) != m_official_kts.second.end()) || 
						  (m_addon_kts.second.find(track_id) != m_addon_kts.second.end()) ||
						  (m_addon_soccers.find(track_id) != m_addon_soccers.end()) ||
						  (m_addon_arenas.find(track_id) != m_addon_arenas.end());

	return peerHasTrack && serverHasTrack;
}  // serverAndPeerHaveTrack
//-----------------------------------------------------------------------------
bool ServerLobby::serverAndPeerHaveKart(std::shared_ptr<STKPeer>& peer, std::string kart_id) const
{
	std::pair<std::set<std::string>, std::set<std::string>> kt = peer->getClientAssets();
	bool peerHasTrack = kt.first.find(kart_id) != kt.first.end();

	bool serverHasTrack = (m_official_kts.first.find(kart_id) != m_official_kts.first.end()) ||
		(m_addon_kts.first.find(kart_id) != m_addon_kts.first.end());
		
	if (peerHasTrack == false)
	{
		std::string message = "Client does not have kart " + kart_id;
		sendStringToPeer(message, peer);
	}
	if (serverHasTrack == false)
	{
		std::string message = "Server does not have kart " + kart_id;
		sendStringToPeer(message, peer);
	}

	return peerHasTrack && serverHasTrack;
} // serverAndPeerHaveTrack
//-----------------------------------------------------------------------------

// difficulty:  Difficulty in server, 0 is beginner, 1 is intermediate, 2 is expert and 3 is supertux (the most difficult).
// game_mode:  Game mode in server, 0 is normal race (grand prix), 1 is time trial (grand prix), 3 is normal race, 4 time trial, 6 is soccer, 7 is free-for-all and 8 is capture the flag.
// soccer_goal_target:  Use goal target in soccer (1 = true, 0 = false)
void ServerLobby::setServerMode(unsigned char difficulty, unsigned char game_mode, unsigned char soccer_goal_target, std::shared_ptr<STKPeer> peer)
{
	ENetEvent *netEvent = new ENetEvent();
	netEvent->channelID = EVENT_CHANNEL_COUNT;
	enet_uint8 data[4] = { 0, difficulty, game_mode, soccer_goal_target }; // difficulty = 3, game mode = 4, soccer goal target = 0
	netEvent->packet = enet_packet_create(data, 4, 0);
	netEvent->type = ENET_EVENT_TYPE_RECEIVE;

	Event *e = new Event(netEvent, peer);
	handleServerConfiguration(e);
	delete e;
} // setServerMode
//-----------------------------------------------------------------------------
bool ServerLobby::stringToServerMode(std::string server_mode_str, unsigned char &out_game_mode, unsigned char &out_soccer_goal_target)
{
	std::vector<std::string> mode_0{ "grand-prix-normal", "gpn", "0" };
	std::vector<std::string> mode_1{ "grand-prix-time", "gpt", "1" };
	std::vector<std::string> mode_3{ "normal", "normal-race", "n", "Waffen", "3" };
	std::vector<std::string> mode_4{ "time", "time-trial", "t", "Zeitrennen", "4" };
	std::vector<std::string> mode_6_0{ "soccer-time", "st", "soccer", "s", "Fu�ball", "Zeitlimit", "6", "6-0" };
	std::vector<std::string> mode_6_1{ "soccer-goal", "sg", "Torlimit", "6-1" };
	std::vector<std::string> mode_7{ "free-for-all", "ffa", "7" };
	std::vector<std::string> mode_8{ "capture-the-flag", "ctf", "8" };

	if (std::find(mode_0.begin(), mode_0.end(), server_mode_str) != mode_0.end())
	{
		out_game_mode = 0; 
		out_soccer_goal_target = 0;
		return true;
	}
	else if (std::find(mode_1.begin(), mode_1.end(), server_mode_str) != mode_1.end())
	{
		out_game_mode = 1;
		out_soccer_goal_target = 0;
		return true;
	}
	else if (std::find(mode_3.begin(), mode_3.end(), server_mode_str) != mode_3.end())
	{
		out_game_mode = 3;
		out_soccer_goal_target = 0;
		return true;
	}
	else if (std::find(mode_4.begin(), mode_4.end(), server_mode_str) != mode_4.end())
	{
		out_game_mode = 4;
		out_soccer_goal_target = 0;
		return true;
	}
	else if (std::find(mode_6_0.begin(), mode_6_0.end(), server_mode_str) != mode_6_0.end())
	{
		out_game_mode = 6;
		out_soccer_goal_target = 0;
		return true;
	}
	else if (std::find(mode_6_1.begin(), mode_6_1.end(), server_mode_str) != mode_6_1.end())
	{
		out_game_mode = 6;
		out_soccer_goal_target = 1;
		return true;
	}
	else if (std::find(mode_6_1.begin(), mode_6_1.end(), server_mode_str) != mode_6_1.end())
	{
		out_game_mode = 6;
		out_soccer_goal_target = 1;
		return true;
	}
	else if (std::find(mode_7.begin(), mode_7.end(), server_mode_str) != mode_7.end())
	{
		out_game_mode = 7;
		out_soccer_goal_target = 0;
		return true;
	}
	else if (std::find(mode_8.begin(), mode_8.end(), server_mode_str) != mode_8.end())
	{
		out_game_mode = 8;
		out_soccer_goal_target = 0;
		return true;
	}

	return false;
} // stringToServerMode
//-----------------------------------------------------------------------------
std::string ServerLobby::serverModeToString(unsigned char game_mode, unsigned char soccer_goal_target)
{
	switch (game_mode)
	{
		case 0:
			return "Normal Race (Grand Prix)";
		case 1:
			return "Time Trial (Grand Prix)";
		case 3:
			return "Normal Race";
		case 4:
			return "Time Trial";
		case 6:
			if (soccer_goal_target == 0)
				return "Soccer (Time Limit)";
			else if (soccer_goal_target == 1)
				return "Soccer (Goal Limit)";
			else
				return "Soccer";
		case 7:
			return "Free for All";
		case 8:
			return "Capture the Flag";
		default:
			return "undefined server mode";
	}
} // serverModeToString
//-----------------------------------------------------------------------------
void ServerLobby::storeResults()
{
#ifdef ENABLE_SQLITE3
    World* w = World::getWorld();
    assert(w);
    std::string records_table_name = ServerConfig::m_records_table_name;
    std::string mode_name = RaceManager::get()->getMinorModeName();
    int player_count = RaceManager::get()->getNumPlayers();
    int laps_number = RaceManager::get()->getNumLaps();
    std::string track_name = RaceManager::get()->getTrackName();
    std::string reverse_string = 
        (RaceManager::get()->getReverseTrack() ? "reverse" : "normal");

    bool record_fetched = false;
    bool record_exists = false;
    double best_result = 0.0;
    std::string best_user = "";

    if (!records_table_name.empty())
    {
        std::string get_query = StringUtils::insertValues("SELECT username, "
            "result FROM %s INNER JOIN "
            "(SELECT venue as v, reverse as r, mode as m, laps as l, "
            "min(result) as min_res FROM %s group by v, r, m, l) "
            "ON venue = v and reverse = r and mode = m and laps = l "
            "and result = min_res "
            "WHERE venue = '%s' and reverse = '%s' "
            "and mode = '%s' and laps = %d;",
            records_table_name.c_str(), records_table_name.c_str(),
            track_name.c_str(), reverse_string.c_str(), mode_name.c_str(),
            laps_number);
        auto ret = vectorSQLQuery(get_query, 2);
        record_fetched = ret.first;
        if (record_fetched && ret.second[0].size() > 0){
            record_exists = true;
            best_user = ret.second[0][0];
            if (!StringUtils::parseString<double>(ret.second[1][0], &best_result))
                record_fetched = false;
        }
    }

    int best_cur_player_idx = -1;
    std::string best_cur_player_name = "";
    double best_cur_time = 1e18;
    for (int i = 0; i < player_count; i++)
    {
        if (w->getKart(i)->isEliminated())
            continue;
        std::string username = StringUtils::wideToUtf8(
            RaceManager::get()->getKartInfo(i).getPlayerName());
        double elapsed_time = RaceManager::get()->getKartRaceTime(i);
        std::stringstream elapsed_string;
        elapsed_string << std::setprecision(4) << std::fixed << elapsed_time;
        if (best_cur_player_idx == -1 || elapsed_time < best_cur_time)
        {
            best_cur_player_idx = i;
            best_cur_time = elapsed_time;
            best_cur_player_name = username;
        }
        std::string query = StringUtils::insertValues(
            "INSERT INTO %s "
            "(username, venue, reverse, mode, laps, result) "
            "VALUES ('%s', '%s', '%s', '%s', %d, '%s');",
            m_results_table_name.c_str(), username.c_str(), track_name.c_str(),
            reverse_string.c_str(), mode_name.c_str(), laps_number, elapsed_string.str()
        );
        easySQLQuery(query);
    }
    if (record_fetched && best_cur_player_idx != -1)
    {
        NetworkString* chat = getNetworkString();
        chat->addUInt8(LE_CHAT);
        chat->setSynchronous(true);
        std::string message;
        if (!record_exists)
        {
            message = StringUtils::insertValues(
                "%s has just set a server record: %s\nThis is the first time set.",
                best_cur_player_name, StringUtils::timeToString(best_cur_time));
        }
        else if (best_result > best_cur_time)
        {
            message = StringUtils::insertValues(
                "%s has just beaten a server record: %s\nPrevious record: %s by %s",
                best_cur_player_name, StringUtils::timeToString(best_cur_time),
                StringUtils::timeToString(best_result), best_user);
        } else {
            delete chat;
            return;
        }
        chat->encodeString16(StringUtils::utf8ToWide(message));
        sendMessageToPeers(chat);
        delete chat;
    }
#endif
}  // storeResults
//-----------------------------------------------------------------------------
void ServerLobby::initAvailableModes()
{
    std::vector<std::string> statements =
        StringUtils::split(ServerConfig::m_available_modes, ' ', false);

    for (const std::string& s: statements)
    {
        if (s.length() <= 1) {
            continue;
        }
        bool difficulty = s[0] == 'd';
        if (difficulty)
        {
            for (unsigned i = 1; i < s.length(); i++)
            {
                m_available_difficulties.insert(s[i] - '0');
            }
        }
        else
        {
            for (unsigned i = 1; i < s.length(); i++)
            {
                m_available_modes.insert(s[i] - '0');
            }
        }
    }
}  // initAvailableModes
//-----------------------------------------------------------------------------
void ServerLobby::resetToDefaultSettings()
{
    handleServerConfiguration(NULL);
    // m_gnu_elimination = false;
}  // resetToDefaultSettings
//-----------------------------------------------------------------------------
void ServerLobby::writeOwnReport(STKPeer* reporter, STKPeer* reporting, const std::string& info)
{
#ifdef ENABLE_SQLITE3
    if (!m_db || !m_player_reports_table_exists)
        return;
    if (!reporter->hasPlayerProfiles())
        return;
    auto reporter_npp = reporter->getPlayerProfiles()[0];

    if (info.empty())
        return;

    if (!reporting->hasPlayerProfiles())
        return;
    auto reporting_npp = reporting->getPlayerProfiles()[0];

    std::string query;
    if (ServerConfig::m_ipv6_connection)
    {
        query = StringUtils::insertValues(
            "INSERT INTO %s "
            "(server_uid, reporter_ip, reporter_ipv6, reporter_online_id, reporter_username, "
            "info, reporting_ip, reporting_ipv6, reporting_online_id, reporting_username) "
            "VALUES (?, %u, \"%s\", %u, ?, ?, %u, \"%s\", %u, ?);",
            ServerConfig::m_player_reports_table.c_str(),
            !reporter->getAddress().isIPv6() ? reporter->getAddress().getIP() : 0,
            reporter->getAddress().isIPv6() ? reporter->getAddress().toString(false) : "",
            reporter_npp->getOnlineId(),
            !reporting->getAddress().isIPv6() ? reporting->getAddress().getIP() : 0,
            reporting->getAddress().isIPv6() ? reporting->getAddress().toString(false) : "",
            reporting_npp->getOnlineId());
    }
    else
    {
        query = StringUtils::insertValues(
            "INSERT INTO %s "
            "(server_uid, reporter_ip, reporter_online_id, reporter_username, "
            "info, reporting_ip, reporting_online_id, reporting_username) "
            "VALUES (?, %u, %u, ?, ?, %u, %u, ?);",
            ServerConfig::m_player_reports_table.c_str(),
            reporter->getAddress().getIP(), reporter_npp->getOnlineId(),
            reporting->getAddress().getIP(), reporting_npp->getOnlineId());
    }
    bool written = easySQLQuery(query,
        [reporter_npp, reporting_npp, info](sqlite3_stmt* stmt)
        {
            // SQLITE_TRANSIENT to copy string
            if (sqlite3_bind_text(stmt, 1, ServerConfig::m_server_uid.c_str(),
                -1, SQLITE_TRANSIENT) != SQLITE_OK)
            {
                Log::error("easySQLQuery", "Failed to bind %s.",
                    ServerConfig::m_server_uid.c_str());
            }
            if (sqlite3_bind_text(stmt, 2,
                StringUtils::wideToUtf8(reporter_npp->getName()).c_str(),
                -1, SQLITE_TRANSIENT) != SQLITE_OK)
            {
                Log::error("easySQLQuery", "Failed to bind %s.",
                    StringUtils::wideToUtf8(reporter_npp->getName()).c_str());
            }
            if (sqlite3_bind_text(stmt, 3,
                info.c_str(),
                -1, SQLITE_TRANSIENT) != SQLITE_OK)
            {
                Log::error("easySQLQuery", "Failed to bind %s.",
                    info.c_str());
            }
            if (sqlite3_bind_text(stmt, 4,
                StringUtils::wideToUtf8(reporting_npp->getName()).c_str(),
                -1, SQLITE_TRANSIENT) != SQLITE_OK)
            {
                Log::error("easySQLQuery", "Failed to bind %s.",
                    StringUtils::wideToUtf8(reporting_npp->getName()).c_str());
            }
        });
    if (written)
    {
        NetworkString* success = getNetworkString();
        success->setSynchronous(true);
        if (reporter == reporting)
            success->addUInt8(LE_REPORT_PLAYER).addUInt8(1)
                .encodeString(m_game_setup->getServerNameUtf8());
        else
            success->addUInt8(LE_REPORT_PLAYER).addUInt8(1)
                .encodeString(reporting_npp->getName());
        reporter->sendPacket(success, true/*reliable*/);
        delete success;
    }
#endif
}   // writeOwnReport
//-----------------------------------------------------------------------------
void ServerLobby::initTournamentPlayers()
{
    // Init categories
    std::vector<std::string> tokens = StringUtils::split(
        ServerConfig::m_soccer_tournament_players, ' ');
    std::string category = "";
    for (std::string& s: tokens)
    {
        if (s.empty())
            continue;
        else if (s[0] == '#')
            category = s.substr(1);
        else
            m_tournament_player_categories[category].push_back(s);
    }

    // Init playing teams
    tokens = StringUtils::split(
        ServerConfig::m_soccer_tournament_match, ' ');
    std::string type = "";
    for (std::string& s: tokens)
    {
        if (s.length() == 1)
            type = s;
        else if (s.empty())
            continue;
        else if (s[0] == '#')
        {
            std::string cat_name = s.substr(1);
            std::set<std::string>& dest = (
                type == "R" ? m_tournament_red_players :
                type == "B" ? m_tournament_blue_players :
                m_tournament_referees);
            for (std::string& member:
                m_tournament_player_categories[cat_name])
                dest.insert(member);
        }
        else if (type == "R") 
            m_tournament_red_players.insert(s);
        else if (type == "B")
            m_tournament_blue_players.insert(s);
        else if (type == "J")
            m_tournament_referees.insert(s);
    }
    m_tournament_init_red = m_tournament_red_players;
    m_tournament_init_blue = m_tournament_blue_players;
    m_tournament_init_ref = m_tournament_referees;

    // Init tournament format
    tokens = StringUtils::split(
        ServerConfig::m_soccer_tournament_rules, ';');
    bool fallback = tokens.size() < 2;
    std::vector<std::string> general;
    if (!fallback)
    {
        general = StringUtils::split(tokens[0], ' ');
        if (general.size() < 4)
            fallback = true;
    }
    if (fallback)
    {
        Log::warn("ServerLobby", "Tournament rules not complete, fallback to default");
        general.clear();
        general.push_back("nochat");
        general.push_back("10");
        general.push_back("GGGGT");
        general.push_back("RRBBR");
        tokens.clear();
        tokens.push_back("nochat 10 GGGT RRBBR");
        tokens.push_back("");
        tokens.push_back("");
        tokens.push_back("not %0");
        tokens.push_back("not %0" " %1");
        tokens.push_back("");
    }
    m_tournament_limited_chat = false;
    m_tournament_length = 10;
    if (general[0] == "nochat")
        m_tournament_limited_chat = true;
    if (StringUtils::parseString<int>(general[1], &m_tournament_length))
    {
        if (m_tournament_length <= 0)
            m_tournament_length = 10;
    }
    else
        m_tournament_length = 10;

    m_tournament_game_limits = general[2];
    m_tournament_colors = general[3];
    m_tournament_max_games = 6;
    for (unsigned i = 0; i < m_tournament_max_games; i++)
        m_tournament_track_filters.emplace_back(tokens[i + 1]);
    }   // initTournamentPlayers
//-----------------------------------------------------------------------------
void ServerLobby::initRaceTournamentPlayers()
{
	std::vector<std::string> tokens = StringUtils::split(
		ServerConfig::m_race_tournament_players, ' ');
	std::string type = "";
	for (std::string& s : tokens)
	{
		if (s.length() == 1)
			type = s;
		else if (type == "P")
			m_race_tournament_players.insert(s);
		else if (type == "J")
			m_race_tournament_referees.insert(s);
	}
}   // initRaceTournamentPlayers
//-----------------------------------------------------------------------------
void ServerLobby::changeColors()
{
    auto peers = STKHost::get()->getPeers();
    for (auto peer : peers)
    {
        if (peer->hasPlayerProfiles())
        {
            auto pp = peer->getPlayerProfiles()[0];
            if (pp->getTeam() == KART_TEAM_RED)
                pp->setTeam(KART_TEAM_BLUE);
            else if (pp->getTeam() == KART_TEAM_BLUE)
                pp->setTeam(KART_TEAM_RED);
        }
    }
    updatePlayerList();
}   // changeColors
//-----------------------------------------------------------------------------
void ServerLobby::sendStringToPeer(std::string& s, std::shared_ptr<STKPeer>& peer) const
{
    NetworkString* chat = getNetworkString();
    chat->addUInt8(LE_CHAT);
    chat->setSynchronous(true);
    chat->encodeString16(StringUtils::utf8ToWide(s));
    peer->sendPacket(chat, true/*reliable*/);
    delete chat;
}   // sendStringToPeer
//-----------------------------------------------------------------------------
void ServerLobby::sendStringToAllPeers(std::string& s)
{
    NetworkString* chat = getNetworkString();
    chat->addUInt8(LE_CHAT);
    chat->setSynchronous(true);
    chat->encodeString16(StringUtils::utf8ToWide(s));
    sendMessageToPeers(chat, true/*reliable*/);
    delete chat;
}   // sendStringToAllPeers
//-----------------------------------------------------------------------------
bool ServerLobby::canRace(std::shared_ptr<STKPeer>& peer) const
{
    return canRace(peer.get());
}   // canRace
//-----------------------------------------------------------------------------
bool ServerLobby::canRace(STKPeer* peer) const
{
    std::string username = StringUtils::wideToUtf8(
        peer->getPlayerProfiles()[0]->getName());

	// Players who do not have the addon defined via /setfield are not allowed to play.
	if (m_set_field != "")
	{
		bool has_addon = false;
		const auto& kt = peer->getClientAssets();

		for (auto& track : kt.second)
		{
			if (track == m_set_field)
			{
				has_addon = true;
				break;
			}
		}

		if (has_addon == false) return false;
	}

	if (m_player_queue_limit > 0)
	{
		for (auto player : peer->getPlayerProfiles())
		{
			std::string player_username = StringUtils::wideToUtf8(player->getName());
			int queueIndex = getQueueIndex(player_username);
			if (queueIndex < 0 || queueIndex >= m_player_queue_limit) return false;
		}
	}

	if (m_gnu_elimination && m_gnu2_activated)
	{
		const auto& kt = peer->getClientAssets();

		for (std::string required_track : m_gnu2_available_tracks)
		{
			bool has_addon = false;
			for (auto& track : kt.second)
			{
				if (track == required_track)
				{
					has_addon = true;
					break;
				}
			}
			if (has_addon == false) return false;
		}

		bool hasGnuKart = kt.first.find(m_gnu_kart) != kt.first.end();
		if (hasGnuKart == false) return false;
	}

    if (ServerConfig::m_soccer_tournament)
    {
        return m_tournament_red_players.count(username) > 0 || 
            m_tournament_blue_players.count(username) > 0;
    }
	else if (ServerConfig::m_race_tournament)
	{
		return m_race_tournament_players.count(username) > 0;
	}
    else if (ServerConfig::m_only_host_riding)
    {
        return peer == m_server_owner.lock().get();
    }
    else if (!m_tracks_queue.empty())
    {
        return peer->getClientAssets().second.count(m_tracks_queue.front());
    }
    else
    {
        return true;
    }
}   // canRace

//-----------------------------------------------------------------------------
bool ServerLobby::hasHostRights(std::shared_ptr<STKPeer>& peer) const
{
    return hasHostRights(peer.get());
}   // hasHostRights
//-----------------------------------------------------------------------------
bool ServerLobby::hasHostRights(STKPeer* peer) const
{
    if (peer == m_server_owner.lock().get())
        return true;
    std::string username = StringUtils::wideToUtf8(
        peer->getPlayerProfiles()[0]->getName());
	if (isVIP(peer))
		return true;
    if (ServerConfig::m_soccer_tournament)
    {
        return m_tournament_referees.count(username) > 0;
    }
	if (ServerConfig::m_race_tournament)
	{
		return m_race_tournament_referees.count(username) > 0;
	}

    return false;
}   // hasHostRights
//-----------------------------------------------------------------------------
bool ServerLobby::voteForCommand(std::shared_ptr<STKPeer>& peer, std::string command)
{
	if (!ServerConfig::m_command_voting) return false;

	std::string username = StringUtils::wideToUtf8(peer->getPlayerProfiles()[0]->getName());
	int playerCount = STKHost::get()->getPeers().size();

	if (m_command_voters.count(command) == 0)
	{
		m_command_voters[command] = std::vector<std::string>();
	}

	if (std::find(m_command_voters[command].begin(), m_command_voters[command].end(), username) != m_command_voters[command].end())
	{
		std::string msg = "You already voted for \"" + command + "\".";
		sendStringToPeer(msg, peer);
	}
	else
	{
		m_command_voters[command].push_back(username);
		std::string message = username + " voted for \"/" + command + "\" (" + std::to_string(m_command_voters[command].size()) + " of " + std::to_string(playerCount) + " votes).";
		sendStringToAllPeers(message);
		Log::info("ServerLobby", message.c_str());
	}
	
	
	if (m_command_voters[command].size() > (playerCount / 2))
	{
		m_command_voters.erase(command);
		return true;
	}
	
	return false;
} // voteForCommand
//-----------------------------------------------------------------------------
bool ServerLobby::commandPermitted(std::string command, std::shared_ptr<STKPeer>& peer, bool hostRights)
{
	if (hostRights)
	{
		return true;
	}
	else
	{
		if (ServerConfig::m_command_voting)
		{
			return voteForCommand(peer, command);
		}
		else
		{
			std::string msg = "You are not server owner";
			sendStringToPeer(msg, peer);
			return false;
		}
	}
} // commandPermitted
//-----------------------------------------------------------------------------
bool ServerLobby::isVIP(std::shared_ptr<STKPeer>& peer) const
{
	return isVIP(peer.get());
}
//-----------------------------------------------------------------------------
bool ServerLobby::isVIP(STKPeer* peer) const
{
	std::string username = StringUtils::wideToUtf8(
		peer->getPlayerProfiles()[0]->getName());

	if (username == "Waldlaubsaengernest" || username == "TheRocker" || username == "re342" || username == "Gelbbrauenlaubsaenger")
		return true;

	return false;
}   // isVIP
//-----------------------------------------------------------------------------
int ServerLobby::getQueueIndex(std::string& username) const
{
	auto it = std::find(m_player_queue.begin(), m_player_queue.end(), username);
	if (it == m_player_queue.end())
		return -1;
	else
		return std::distance(m_player_queue.begin(), it);
}

stringw ServerLobby::getQueueNumberIcon(std::string& username) const
{
	int queueIndex = getQueueIndex(username);
	if (queueIndex < 0)
		return L"";

	if (queueIndex < m_player_queue_limit && queueIndex < 20)
	{
		stringw icon = StringUtils::utf32ToWide({ 0x2460 + (char32_t) queueIndex });
		return icon;
	}
	else
	{
		std::wstring icon = L"#" + std::to_wstring(queueIndex + 1) + L" ";
		return icon.c_str();
	}
}
//-----------------------------------------------------------------------------
void ServerLobby::addDeletePlayersFromQueue(std::shared_ptr<STKPeer>& peer, bool add)
{
	if (peer == NULL) return;

	for (auto &player : peer->getPlayerProfiles())
	{
		std::string username = StringUtils::wideToUtf8(player->getName());

		if (add) // add players to queue
		{
			if (std::find(m_player_queue.begin(), m_player_queue.end(), username) == m_player_queue.end())
				m_player_queue.push_back(username);
		}
		else // delete players from queue
		{
			int queueIndex = getQueueIndex(username);
			if (queueIndex != -1)
				m_player_queue.erase(m_player_queue.begin() + queueIndex);
		}
	}
}
//-----------------------------------------------------------------------------
void ServerLobby::rotatePlayerQueue()
{
	if (m_player_queue.size() <= m_player_queue_limit) return;

	for (int i = 0; i < m_player_queue_limit; i++)
		m_player_queue.push_back(m_player_queue[i]);
	
	m_player_queue.erase(m_player_queue.begin(), m_player_queue.begin() + m_player_queue_limit);

	if (m_player_queue_limit == 2 && m_player_queue.size() >= 2)
	{
		std::shared_ptr<NetworkPlayerProfile> player1 = NULL, player2 = NULL;
		auto peers = STKHost::get()->getPeers();
		for (auto peer : peers)
		{
			for (auto player : peer->getPlayerProfiles())
			{
				if (StringUtils::wideToUtf8(player->getName()) == m_player_queue[0]) 
					player1 = player;
				else if (StringUtils::wideToUtf8(player->getName()) == m_player_queue[1]) 
					player2 = player;
			}
		}
		if ((player1->getTeam() == KART_TEAM_RED) && (player2->getTeam() == KART_TEAM_RED))
			player2->setTeam(KART_TEAM_BLUE);
		if ((player1->getTeam() == KART_TEAM_RED) && (player2->getTeam() == KART_TEAM_RED))
			player2->setTeam(KART_TEAM_BLUE);
	}

	updatePlayerList();
}
//-----------------------------------------------------------------------------
bool ServerLobby::teamsBalanced()
{
	auto peers = STKHost::get()->getPeers();
	int red = 0, blue = 0;

	if (m_player_queue_limit > 0)
	{
		for (auto peer : peers)
		{
			for (auto player : peer->getPlayerProfiles())
			{
				std::string player_username = StringUtils::wideToUtf8(player->getName());
				int queueIdx = getQueueIndex(player_username);
				if (queueIdx >= 0 && queueIdx < m_player_queue_limit)
				{
					if (player->getTeam() == KART_TEAM_RED) red++;
					else if (player->getTeam() == KART_TEAM_BLUE) blue++;
				}
			}
		}
	}
	else
	{
		for (auto peer : peers)
		{
			if (peer->alwaysSpectate()) continue;

			for (auto player : peer->getPlayerProfiles())
			{
				if (player->getTeam() == KART_TEAM_RED) red++;
				else if (player->getTeam() == KART_TEAM_BLUE) blue++;
			}
		}
	}

	return (red > 0 && blue > 0) || (red + blue == 1);
}
//-----------------------------------------------------------------------------
void ServerLobby::loadTracksQueueFromConfig()
{
    std::vector<std::string> tokens = StringUtils::split(
        ServerConfig::m_tracks_order, ' ');
    m_tracks_queue.clear();
    for (std::string& s: tokens)
        m_tracks_queue.push_back(s);
}   // loadTracksQueueFromConfig
//-----------------------------------------------------------------------------
void ServerLobby::sendGnuStandingsToPeer(std::shared_ptr<STKPeer> peer) const
{
    std::string result = "Gnu Elimination ";
    if (m_gnu_elimination)
        result += "is running";
    else
        result += "is disabled";
    if (!m_gnu_participants.empty())
        result += ", standings:";
    for (int i = 0; i < (int)m_gnu_participants.size(); i++)
    {
        std::string line = "\n" + (i < m_gnu_remained ?
            std::to_string(i + 1) : "[" + std::to_string(i + 1) + "]");
        line += ". " + m_gnu_participants[i];
        result += line;
    }
    sendStringToPeer(result, peer);
}   // sendGnuStandingsToPeer
//-----------------------------------------------------------------------------
void ServerLobby::sendGrandPrixStandingsToPeer(std::shared_ptr<STKPeer> peer) const
{
    std::vector<std::pair<GPScore, std::string>> results;
    for (auto& p: m_gp_scores)
        results.emplace_back(p.second, p.first);
    std::stable_sort(results.rbegin(), results.rend());
    std::stringstream response;
    response << "Grand Prix standings\n";
    for (int i = 0; i < results.size(); i++)
    {
        response << (i + 1) << ". ";
        response << "  " << results[i].second;
        response << "  " << results[i].first.score;
        response << "  " << "(" << StringUtils::timeToString(results[i].first.time) << ")";
        response << "\n";
    }
    std::string answer = response.str();
    sendStringToPeer(answer, peer);
}   // sendGnuStandingsToPeer
//-----------------------------------------------------------------------------
void ServerLobby::loadCustomScoring()
{
    m_scoring_int_params.clear();
    m_scoring_type = "";
    std::string scoring = ServerConfig::m_gp_scoring;
    if (!scoring.empty())
    {
        std::vector<std::string> params = StringUtils::split(scoring, ' ');
        if (params.empty())
            return;    
        m_scoring_type = params[0];
        for (unsigned i = 1; i < params.size(); i++)
        {
            int param;
            if (!StringUtils::fromString(params[i], param))
            {
                Log::warn("ServerLobby", "Unable to parse integer from custom scoring data");
                return;
            }
            m_scoring_int_params.push_back(param);
        }
    }
}   // loadCustomScoring
//-----------------------------------------------------------------------------   
void ServerLobby::updateWorldSettings()
{
    WorldWithRank *wwr = dynamic_cast<WorldWithRank*>(World::getWorld());
    if (wwr)
        wwr->setCustomScoringSystem(m_scoring_type, m_scoring_int_params);
    SoccerWorld *sw = dynamic_cast<SoccerWorld*>(World::getWorld());
    if (sw)
    {
        std::string policy = ServerConfig::m_soccer_goals_policy;
        if (policy == "standard")
            sw->setGoalScoringPolicy(0);
        else if (policy == "no-own-goals")
            sw->setGoalScoringPolicy(1);
        else if (policy == "advanced")
            sw->setGoalScoringPolicy(2);
        else
            Log::warn("ServerLobby", "Soccer goals policy %s does not exist", policy.c_str());
    }
}   // updateWorldSettings
//-----------------------------------------------------------------------------   
void ServerLobby::loadWhiteList()
{
    std::vector<std::string> tokens = StringUtils::split(
        ServerConfig::m_white_list, ' ');
    for (std::string& s: tokens)
        m_usernames_white_list.insert(s);
}   // loadWhiteList
//-----------------------------------------------------------------------------   
void ServerLobby::changeLimitForTournament(bool goal_target)
{
    m_game_setup->setSoccerGoalTarget(goal_target);
    NetworkString* server_info = getNetworkString();
    server_info->setSynchronous(true);
    server_info->addUInt8(LE_SERVER_INFO);
    m_game_setup->addServerInfo(server_info);
    sendMessageToPeers(server_info);
    delete server_info;
    updatePlayerList();
}   // changeLimitForTournament
//-----------------------------------------------------------------------------
/*
Tournament states are defined by taking the number of game modulo 8.
Below for each state the limit, colors (for first and second teams) and 
icy / non-icy (listed as "addon") arena choice, are isted:
0: time, red blue, addon
1: time, red blue, icy
2: time, blue red, addon
3: time, blue red, icy
4: goals, red blue, icy
5: goals, red blue, addon
6: goals, blue red, icy
7: goals, blue red, addon
*/
//-----------------------------------------------------------------------------
bool ServerLobby::tournamentGoalsLimit(int game) const
{
    return m_tournament_game_limits[game] == 'G';
    // int rem = game % 8;
    // if (rem < 0)
    //     rem += 8;
    // return (rem >> 2) & 1;
}   // tournamentGoalsLimit
//-----------------------------------------------------------------------------
bool ServerLobby::tournamentColorsSwapped(int game) const
{
    return m_tournament_colors[game] == 'B';
    // int rem = game % 8;
    // if (rem < 0)
    //     rem += 8;
    // return (rem >> 1) & 1;
}   // tournamentColorsSwapped

bool ServerLobby::tournamentHasIcy(int game) const
{
	int rem = game % 8;
	if (rem < 0)
		rem += 8;
	return (rem == 1) || (rem > 5);
}   // tournamentHasIcy
//-----------------------------------------------------------------------------
bool ServerLobby::tournamentHasTournamentField(int game) const
{
	int rem = game % 8;
	if (rem < 0)
		rem += 8;
	printf("hi");
	return (rem == 5);
}   // tournamentHasTournamentField

bool ServerLobby::tournamentHasGrass(int game) const
{
        int rem = game % 8;
        if (rem < 0)
                rem += 8;
        return (rem == 3);
}   // tournamentHasTournamentField


//-----------------------------------------------------------------------------
// bool ServerLobby::tournamentHasIcy(int game) const
// {
//     int rem = game % 8;
//     if (rem < 0)
//         rem += 8;
//     return (rem ^ (rem >> 2)) & 1;
// }   // tournamentHasIcy
//-----------------------------------------------------------------------------
void ServerLobby::initAvailableTracks()
{
    m_global_filter = TrackFilter(ServerConfig::m_only_played_tracks_string);
    m_must_have_tracks = StringUtils::split(
        ServerConfig::m_must_have_tracks_string, ' ', false);
    /*m_inverted_config_restriction = false;
    m_restricting_config = true;
    if (((std::string)(ServerConfig::m_only_played_tracks_string)).empty())
    {
        m_restricting_config = false;
        return;
    }
    std::vector<std::string> available_tracks =
        StringUtils::split(ServerConfig::m_only_played_tracks_string,
        ' ', false);

    for (unsigned i = 0; i < available_tracks.size(); i++)
        if (available_tracks[i] == "not")
            m_inverted_config_restriction = true;

    for (unsigned i = 0; i < available_tracks.size(); i++)
    {
        if (available_tracks[i] == "not")
            continue;
        int separator = available_tracks[i].find(':');
        if (separator != std::string::npos)
        {
            std::string track = available_tracks[i].substr(0, separator);
            std::string params_str = available_tracks[i].substr(separator + 1);
            std::vector<std::string> params = StringUtils::split(
                params_str, ',', false);
            m_config_track_limitations[track] = params;
        }
        else
            m_config_available_tracks.insert(available_tracks[i]);
    }*/
}   // initAvailableTracks
//-----------------------------------------------------------------------------
// int ServerLobby::getTrackMaxPlayers(std::string& name) const
// {
//     auto map_entry = m_config_track_limitations.find(name);
//     if (map_entry == m_config_track_limitations.end())
//         return INT_MAX;
//     std::string key = "max_players";
//     auto map_value = map_entry->second;
//     auto where = std::find(map_value.begin(), map_value.end(), key);
//     auto end = map_value.end();
//     if (where == end)
//         return INT_MAX;
//     where++;
//     if (where == end)
//         return INT_MAX;
//     int track_max_players;
//     std::string max_str = *where;
//     if (!StringUtils::parseString<int>(max_str, &track_max_players))
//     {
//         Log::warn("ServerLobby", "Bad max_players value for track %s", name.c_str());
//         return INT_MAX;
//     }
//     return track_max_players;
// }   // getTrackMaxPlayers
//-----------------------------------------------------------------------------

#ifdef ENABLE_WEB_SUPPORT

void ServerLobby::loadAllTokens()
{
#ifdef ENABLE_SQLITE3
    std::string tokens_table_name = ServerConfig::m_tokens_table;
    std::string get_query = StringUtils::insertValues(
        "SELECT distinct tokens from %s;",
        tokens_table_name.c_str());
    auto ret = vectorSQLQuery(get_query, 1);
    if (!ret.first)
    {
        Log::warn("ServerLobby", "Could not make a query to retrieve tokens.");
    }
    else if (ret.second[0].size() > 0)
    {
        Log::info("ServerLobby", "Successfully loaded %d tokens.", (int)ret.second[0].size());
        for (std::string& s: ret.second[0])
            m_web_tokens.insert(s);
    }
#endif
}   // loadAllTokens
//-----------------------------------------------------------------------------

std::string ServerLobby::getToken()
{
    int tries = m_token_generation_tries.load();
    m_token_generation_tries.store(tries + 1);
    std::mt19937 mt(time(nullptr) + tries);
    std::string token;
    for (int i = 0; i < 16; ++i) {
        int z = mt() % 36;
        if (z < 26)
            token.push_back('a' + z);
        else
            token.push_back('0' + z - 26);
        if ((i & 3) == 3)
            token.push_back(' ');
    }
    token.pop_back();
    return token;
}   // getToken
//-----------------------------------------------------------------------------

#endif // ENABLE_WEB_SUPPORT


