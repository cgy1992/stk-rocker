//
//  SuperTuxKart - a fun racing game with go-kart
//  Copyright (C) 2018 SuperTuxKart-Team
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

#ifndef SERVER_LOBBY_HPP
#define SERVER_LOBBY_HPP

#include "network/protocols/lobby_protocol.hpp"
#include "utils/cpp2011.hpp"
#include "utils/time.hpp"
#include "utils/track_filter.hpp"

#include "irrString.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <deque>

#ifdef ENABLE_SQLITE3
#include <sqlite3.h>
#endif

class BareNetworkString;
class NetworkItemManager;
class NetworkString;
class NetworkPlayerProfile;
class STKPeer;
class SocketAddress;

namespace Online
{
    class Request;
}

// I know it should be in a more suitable place, but for now I have no idea
// how to make this with the current system. Sorry. Hope to refactor later.

struct GPScore
{
    int score = 0;
    double time = 0.;
    bool operator < (const GPScore& rhs) const
    {
        return (score < rhs.score || (score == rhs.score && time > rhs.time));
    }
    bool operator > (const GPScore& rhs) const
    {
        return (score > rhs.score || (score == rhs.score && time < rhs.time));
    }
};

class ServerLobby : public LobbyProtocol
{
public:
    /* The state for a small finite state machine. */
    enum ServerState : unsigned int
    {
        SET_PUBLIC_ADDRESS,       // Waiting to receive its public ip address
        REGISTER_SELF_ADDRESS,    // Register with STK online server
        WAITING_FOR_START_GAME,   // In lobby, waiting for (auto) start game
        SELECTING,                // kart, track, ... selection started
        LOAD_WORLD,               // Server starts loading world
        WAIT_FOR_WORLD_LOADED,    // Wait for clients and server to load world
        WAIT_FOR_RACE_STARTED,    // Wait for all clients to have started the race
        RACING,                   // racing
        WAIT_FOR_RACE_STOPPED,    // Wait server for stopping all race protocols
        RESULT_DISPLAY,           // Show result screen
        ERROR_LEAVE,              // shutting down server
        EXITING
    };
private:
    struct KeyData
    {
        std::string m_aes_key;
        std::string m_aes_iv;
        irr::core::stringw m_name;
        std::string m_country_code;
        bool m_tried = false;
    };
    bool m_player_reports_table_exists;

#ifdef ENABLE_SQLITE3
    sqlite3* m_db;

    std::string m_server_stats_table;

    std::string m_results_table_name;

    bool m_ip_ban_table_exists;

    bool m_ipv6_ban_table_exists;

    bool m_online_id_ban_table_exists;

    bool m_ip_geolocation_table_exists;

    bool m_ipv6_geolocation_table_exists;

    uint64_t m_last_poll_db_time;

    void pollDatabase();

    bool easySQLQuery(const std::string& query,
        std::function<void(sqlite3_stmt* stmt)> bind_function = nullptr) const;

    std::pair<bool, std::vector<std::vector<std::string>>>
        vectorSQLQuery(const std::string& query, int columns,
        std::function<void(sqlite3_stmt* stmt)> bind_function = nullptr) const;

    void checkTableExists(const std::string& table, bool& result);

    std::string ip2Country(const SocketAddress& addr) const;

    std::string ipv62Country(const SocketAddress& addr) const;
#endif
    void initDatabase();

    void destroyDatabase();

    std::atomic<ServerState> m_state;

    /* The state used in multiple threads when reseting server. */
    enum ResetState : unsigned int
    {
        RS_NONE, // Default state
        RS_WAITING, // Waiting for reseting finished
        RS_ASYNC_RESET // Finished reseting server in main thread, now async
                       // thread
    };

    std::atomic<ResetState> m_rs_state;

    /** Hold the next connected peer for server owner if current one expired
     * (disconnected). */
    std::weak_ptr<STKPeer> m_server_owner;

    /** AI peer which holds the list of reserved AI for dedicated server. */
    std::weak_ptr<STKPeer> m_ai_peer;

    /** AI profiles for all-in-one graphical client server, this will be a
     *  fixed count thorough the live time of server, which its value is
     *  configured in NetworkConfig. */
    std::vector<std::shared_ptr<NetworkPlayerProfile> > m_ai_profiles;

    std::atomic<uint32_t> m_server_owner_id;

    /** Official karts and tracks available in server. */
    std::pair<std::set<std::string>, std::set<std::string> > m_official_kts;

    /** Addon karts and tracks available in server. */
    std::pair<std::set<std::string>, std::set<std::string> > m_addon_kts;

    /** Addon arenas available in server. */
    std::set<std::string> m_addon_arenas;

    /** Addon soccers available in server. */
    std::set<std::string> m_addon_soccers;

    /** Available karts and tracks for all clients, this will be initialized
     *  with data in server first. */
    std::pair<std::set<std::string>, std::set<std::string> > m_available_kts;

    /** Available karts and tracks for all clients, this will be initialized
     *  with data in server first. */
    std::pair<std::set<std::string>, std::set<std::string> > m_entering_kts;

    /** Keeps track of the server state. */
    std::atomic_bool m_server_has_loaded_world;

    bool m_registered_for_once_only;

    bool m_save_server_config;

    /** Counts how many peers have finished loading the world. */
    std::map<std::weak_ptr<STKPeer>, bool,
        std::owner_less<std::weak_ptr<STKPeer> > > m_peers_ready;

    std::weak_ptr<Online::Request> m_server_registering;

    /** Timeout counter for various state. */
    std::atomic<int64_t> m_timeout;

    std::mutex m_keys_mutex;

    std::map<uint32_t, KeyData> m_keys;

    std::map<std::weak_ptr<STKPeer>,
        std::pair<uint32_t, BareNetworkString>,
        std::owner_less<std::weak_ptr<STKPeer> > > m_pending_connection;

    std::map<std::string, uint64_t> m_pending_peer_connection;

    /* Ranking related variables */
    // If updating the base points, update the base points distribution in DB
    const double BASE_RANKING_POINTS   = 4000.0;
    const double MAX_SCALING_TIME      = 500.0;
    const double MAX_POINTS_PER_SECOND = 0.125;
    const double HANDICAP_OFFSET       = 1000.0;

    /** Online id to profile map, handling disconnection in ranked server */
    std::map<uint32_t, std::weak_ptr<NetworkPlayerProfile> > m_ranked_players;

    /** Multi-session ranking scores for each current player */
    std::map<uint32_t, double> m_scores;

    /** The maximum ranking scores achieved for each current player */
    std::map<uint32_t, double> m_max_scores;

    /** Number of ranked races done for each current players */
    std::map<uint32_t, unsigned> m_num_ranked_races;

    /* Saved the last game result */
    NetworkString* m_result_ns;

    /* Used to make sure clients are having same item list at start */
    BareNetworkString* m_items_complete_state;

    std::atomic<uint32_t> m_server_id_online;

    std::atomic<uint32_t> m_client_server_host_id;

    std::atomic<int> m_difficulty;

    std::atomic<int> m_game_mode;

    std::atomic<int> m_lobby_players;

    std::atomic<uint64_t> m_last_success_poll_time;

    uint64_t m_last_unsuccess_poll_time, m_server_started_at, m_server_delay;

    // Default game settings if no one has ever vote, and save inside here for
    // final vote (for live join)
    PeerVote* m_default_vote;

    int m_battle_hit_capture_limit;

    float m_battle_time_limit;

    unsigned m_item_seed;

    uint32_t m_winner_peer_id;

    uint64_t m_client_starting_time;

    // Calculated before each game started
    unsigned m_ai_count;

    std::vector<std::string> m_must_have_tracks;

    bool m_restricting_config;

    bool m_inverted_config_restriction;

    std::set<std::string> m_config_available_tracks;

    std::map<std::string, std::vector<std::string>> m_config_track_limitations;

    irr::core::stringw m_help_message;

    std::string m_available_commands;

    std::map<STKPeer*, std::set<irr::core::stringw>> m_message_receivers;

    std::set<STKPeer*> m_team_speakers;

    bool m_gnu_elimination;

    int m_gnu_remained;

    std::string m_gnu_kart;

    std::vector<std::string> m_gnu_participants;
    
    bool m_gnu2_activated = false;

	bool m_gnu2_initialized = false;

	std::vector<std::string> m_gnu2_available_tracks;

    std::set<int> m_available_difficulties;

    std::set<int> m_available_modes;

    std::map<std::string, std::vector<std::string>> m_tournament_player_categories;

    std::set<std::string> m_tournament_red_players;

    std::set<std::string> m_tournament_blue_players;
    
    std::set<std::string> m_tournament_referees;

    std::set<std::string> m_tournament_init_red;

    std::set<std::string> m_tournament_init_blue;

    std::set<std::string> m_tournament_init_ref;
    
	std::set<std::string> m_race_tournament_players;

	std::set<std::string> m_race_tournament_referees;

    bool m_tournament_limited_chat;

    int m_tournament_length;

    int m_tournament_max_games;

    std::string m_tournament_game_limits;

    std::string m_tournament_colors;

    std::vector<std::string> m_tournament_arenas;

    std::vector<TrackFilter> m_tournament_track_filters;

    TrackFilter m_global_filter;

    std::set<std::string> m_temp_banned;

    std::deque<std::string> m_tracks_queue;

    std::map<std::string, GPScore> m_gp_scores;

    int m_tournament_game;

    int m_fixed_lap;

    std::vector<int> m_scoring_int_params;

    std::string m_scoring_type;

    std::set<STKPeer*> m_default_always_spectate_peers;

    std::set<std::string> m_usernames_white_list;

    bool m_allowed_to_start;

#ifdef ENABLE_WEB_SUPPORT
    std::set<std::string> m_web_tokens;

    std::atomic<int> m_token_generation_tries;
#endif
    std::string m_set_field;

	std::map<std::string, std::string> m_set_kart;

	std::map<std::string, std::vector<std::string>> m_command_voters; // m_command_votes[command][usernames]

	int m_player_queue_limit = -1;
	bool m_player_queue_rotable = false;
	std::vector<std::string> m_player_queue;


    // connection management
    void clientDisconnected(Event* event);
    void connectionRequested(Event* event);
    // kart selection
    void kartSelectionRequested(Event* event);
    // Track(s) votes
    void handlePlayerVote(Event *event);
    void playerFinishedResult(Event *event);
    void registerServer();
    void finishedLoadingWorldClient(Event *event);
    void finishedLoadingLiveJoinClient(Event *event);
    void kickHost(Event* event);
    void changeTeam(Event* event);
    void handleChat(Event* event);
    void unregisterServer(bool now,
        std::weak_ptr<ServerLobby> sl = std::weak_ptr<ServerLobby>());
    void updatePlayerList(bool update_when_reset_server = false);
    void updateServerOwner();
    void handleServerConfiguration(Event* event);
    void updateTracksForMode();
    bool checkPeersReady(bool ignore_ai_peer) const;
    void resetPeersReady()
    {
        for (auto it = m_peers_ready.begin(); it != m_peers_ready.end();)
        {
            if (it->first.expired())
            {
                it = m_peers_ready.erase(it);
            }
            else
            {
                it->second = false;
                it++;
            }
        }
    }
    void addPeerConnection(const std::string& addr_str)
    {
        m_pending_peer_connection[addr_str] = StkTime::getMonoTimeMs();
    }
    void removeExpiredPeerConnection()
    {
        // Remove connect to peer protocol running more than a 45 seconds
        // (from stk addons poll server request),
        for (auto it = m_pending_peer_connection.begin();
             it != m_pending_peer_connection.end();)
        {
            if (StkTime::getMonoTimeMs() - it->second > 45000)
                it = m_pending_peer_connection.erase(it);
            else
                it++;
        }
    }
    void replaceKeys(std::map<uint32_t, KeyData>& new_keys)
    {
        std::lock_guard<std::mutex> lock(m_keys_mutex);
        std::swap(m_keys, new_keys);
    }
    void handlePendingConnection();
    void handleUnencryptedConnection(std::shared_ptr<STKPeer> peer,
                                     BareNetworkString& data,
                                     uint32_t online_id,
                                     const irr::core::stringw& online_name,
                                     bool is_pending_connection,
                                     std::string country_code = "");
    bool decryptConnectionRequest(std::shared_ptr<STKPeer> peer,
                                  BareNetworkString& data,
                                  const std::string& key,
                                  const std::string& iv,
                                  uint32_t online_id,
                                  const irr::core::stringw& online_name,
                                  const std::string& country_code);
    bool handleAllVotes(PeerVote* winner, uint32_t* winner_peer_id);
    void getRankingForPlayer(std::shared_ptr<NetworkPlayerProfile> p);
    void submitRankingsToAddons();
    void computeNewRankings();
    void clearDisconnectedRankedPlayer();
    double computeRankingFactor(uint32_t online_id);
    double distributeBasePoints(uint32_t online_id);
    double getModeFactor();
    double getModeSpread();
    double getTimeSpread(double time);
    double getUncertaintySpread(uint32_t online_id);
    double scalingValueForTime(double time);
    void checkRaceFinished();
    void getHitCaptureLimit();
    void configPeersStartTime();
    void resetServer();
    void addWaitingPlayersToGame();
    void changeHandicap(Event* event);
    void handlePlayerDisconnection() const;
    void addLiveJoinPlaceholder(
        std::vector<std::shared_ptr<NetworkPlayerProfile> >& players) const;
    NetworkString* getLoadWorldMessage(
        std::vector<std::shared_ptr<NetworkPlayerProfile> >& players,
        bool live_join) const;
    void encodePlayers(BareNetworkString* bns,
        std::vector<std::shared_ptr<NetworkPlayerProfile> >& players) const;
    std::vector<std::shared_ptr<NetworkPlayerProfile> > getLivePlayers() const;
    void setPlayerKarts(const NetworkString& ns, STKPeer* peer) const;
    bool handleAssets(const NetworkString& ns, STKPeer* peer);
    void handleServerCommand(Event* event, std::shared_ptr<STKPeer> peer);
    void liveJoinRequest(Event* event);
    void rejectLiveJoin(STKPeer* peer, BackLobbyReason blr);
    bool canLiveJoinNow() const;
    bool worldIsActive() const;
    int getReservedId(std::shared_ptr<NetworkPlayerProfile>& p,
                      unsigned local_id) const;
    void handleKartInfo(Event* event);
    void clientInGameWantsToBackLobby(Event* event);
    void clientSelectingAssetsWantsToBackLobby(Event* event);
    void kickPlayerWithReason(STKPeer* peer, const char* reason) const;
    void testBannedForIP(STKPeer* peer) const;
    void testBannedForIPv6(STKPeer* peer) const;
    void testBannedForOnlineId(STKPeer* peer, uint32_t online_id) const;
    void writeDisconnectInfoTable(STKPeer* peer);
    void writePlayerReport(Event* event);
    bool supportsAI();
    void updateGnuElimination();
    void updateAddons();
    void initTournamentPlayers();
    void initRaceTournamentPlayers();
    void changeColors();
    void sendStringToPeer(std::string& s, std::shared_ptr<STKPeer>& peer) const;
    void sendStringToAllPeers(std::string& s);
    bool canRace(std::shared_ptr<STKPeer>& peer) const;
    bool canRace(STKPeer* peer) const;
    bool hasHostRights(std::shared_ptr<STKPeer>& peer) const;
    bool hasHostRights(STKPeer* peer) const;
    bool voteForCommand(std::shared_ptr<STKPeer>& peer, std::string command);
	bool commandPermitted(std::string command, std::shared_ptr<STKPeer>& peer, bool hostRights);
	bool isVIP(std::shared_ptr<STKPeer>& peer) const;
	bool isVIP(STKPeer* peer) const;
	int getQueueIndex(std::string &username) const;
	irr::core::stringw getQueueNumberIcon(std::string &username) const;
	void addDeletePlayersFromQueue(std::shared_ptr<STKPeer>& peer, bool add);
	void rotatePlayerQueue();
	void init1vs1Ranking();
	bool teamsBalanced();
    void loadTracksQueueFromConfig();
    void sendGnuStandingsToPeer(std::shared_ptr<STKPeer> peer) const;
    void sendGrandPrixStandingsToPeer(std::shared_ptr<STKPeer> peer) const;
    void loadCustomScoring();
    void updateWorldSettings();
    void loadWhiteList();
    void changeLimitForTournament(bool goal_target);
    bool tournamentGoalsLimit(int game) const;
    bool tournamentColorsSwapped(int game) const;
    bool tournamentHasIcy(int game) const;
    bool tournamentHasTournamentField(int game) const;
    bool tournamentHasGrass(int game) const;
    void selectRandomTracks(std::vector<std::string>& tracks, int newTrackCount);
	void selectRandomTracks(std::vector<std::string> &tracks, std::vector<std::string> &availableTracks, int newTrackCount);
	bool serverAndPeerHaveTrack(std::shared_ptr<STKPeer>& peer, std::string track_id) const;
	bool serverAndPeerHaveTrack(STKPeer* peer, std::string track_id) const;
	bool serverAndPeerHaveKart(std::shared_ptr<STKPeer>& peer, std::string track_id) const;
	void setServerMode(unsigned char difficulty, unsigned char game_mode, unsigned char soccer_goal_target, std::shared_ptr<STKPeer> peer);
	bool stringToServerMode(std::string server_mode_str, unsigned char &out_game_mode, unsigned char &out_soccer_goal_target);
	std::string serverModeToString(unsigned char game_mode, unsigned char soccer_goal_target);
#ifdef ENABLE_WEB_SUPPORT
    void loadAllTokens();
    std::string getToken();
#endif
public:
             ServerLobby();
    virtual ~ServerLobby();

    virtual bool notifyEventAsynchronous(Event* event) OVERRIDE;
    virtual bool notifyEvent(Event* event) OVERRIDE;
    virtual void setup() OVERRIDE;
    virtual void update(int ticks) OVERRIDE;
    virtual void asynchronousUpdate() OVERRIDE;

    void startSelection(const Event *event=NULL);
    void checkIncomingConnectionRequests();
    void finishedLoadingWorld() OVERRIDE;
    ServerState getCurrentState() const { return m_state.load(); }
    void updateBanList();
    bool waitingForPlayers() const;
    virtual bool allPlayersReady() const OVERRIDE
                            { return m_state.load() >= WAIT_FOR_RACE_STARTED; }
    virtual bool isRacing() const OVERRIDE { return m_state.load() == RACING; }
    bool allowJoinedPlayersWaiting() const;
    void setSaveServerConfig(bool val)          { m_save_server_config = val; }
    float getStartupBoostOrPenaltyForKart(uint32_t ping, unsigned kart_id);
    int getDifficulty() const                   { return m_difficulty.load(); }
    int getGameMode() const                      { return m_game_mode.load(); }
    int getLobbyPlayers() const              { return m_lobby_players.load(); }
    void saveInitialItems(std::shared_ptr<NetworkItemManager> nim);
    void saveIPBanTable(const SocketAddress& addr);
    void listBanTable();
    void initServerStatsTable();
    bool isAIProfile(const std::shared_ptr<NetworkPlayerProfile>& npp) const
    {
        return std::find(m_ai_profiles.begin(), m_ai_profiles.end(), npp) !=
            m_ai_profiles.end();
    }
    void storeResults();
    uint32_t getServerIdOnline() const           { return m_server_id_online; }
    void setClientServerHostId(uint32_t id)   { m_client_server_host_id = id; }
    void initAvailableTracks();
    void initAvailableModes();
    void resetToDefaultSettings();
    void writeOwnReport(STKPeer* reporter, STKPeer* reporting, const std::string& info);
    // int getTrackMaxPlayers(std::string& name) const;
};   // class ServerLobby

#endif // SERVER_LOBBY_HPP
