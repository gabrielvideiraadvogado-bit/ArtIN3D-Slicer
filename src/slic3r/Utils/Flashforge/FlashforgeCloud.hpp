#pragma once

// ArtIN3D: Flashforge account (cloud) access through the artin3d-ffbridge helper.
//
// Flashforge's network library ships only as a closed x86_64 binary inside Flash Studio,
// so it runs in a separate helper process under Rosetta (see src/artin3d_ffbridge). This
// class owns that process and the account session (tokens in AppConfig section
// "flashforge_cloud"). Requests block: call them off the UI thread. Real-time events
// (printer detail, connection state) arrive on a reader thread through the event handler.

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Slic3r {

class FlashforgeCloud
{
public:
    using EventHandler = std::function<void(const nlohmann::json& event)>;

    static FlashforgeCloud& instance();

    // Whether this Mac can reach the Flashforge cloud: the helper ships with the app and
    // Flash Studio (which provides the library) is installed. `reason` explains a false.
    bool available(std::string& reason);

    bool logged_in() const;
    // Session data the Flashforge web login page hands over (homepage_get_token).
    bool login_with_token(const std::string& access_token, const std::string& refresh_token, int64_t expires_in,
                          int64_t start_time, std::string& error);
    void logout();
    nlohmann::json profile() const;

    // Printers bound to the account, each with its current detail (status, temperatures,
    // material station). Empty array and `error` set on failure.
    nlohmann::json printers_with_detail(std::string& error);

    // Real-time connection: printers push their detail as "detail" events.
    void set_event_handler(EventHandler handler);
    bool connect_realtime(const std::vector<std::string>& dev_topics, std::string& error);
    bool request_detail(std::string& error);
    // action: "light" {on}, "camera" {open}, "loadFilament"/"unloadFilament" {slot}
    bool command(const std::string& dev_topic, const std::string& action, const nlohmann::json& args, std::string& error);

    // Uploads a sliced file to the printer through the Flashforge cloud and creates the print
    // job (started right away when job["printNow"]). job: filePath, thumbPath, dstName,
    // printNow, leveling, timeLapse, useMatlStation, mappings[{toolId, slotId, materialName,
    // toolColor, slotColor}]. Progress arrives as "sendProgress" events.
    bool send_file(const nlohmann::json& printer, const nlohmann::json& job, std::string& error);

private:
    FlashforgeCloud() = default;
    ~FlashforgeCloud();
    FlashforgeCloud(const FlashforgeCloud&) = delete;
    FlashforgeCloud& operator=(const FlashforgeCloud&) = delete;

    bool           start_bridge(std::string& error);
    void           stop_bridge();
    void           read_loop(void* stream);
    nlohmann::json call(const std::string& method, const nlohmann::json& params, std::string& error, int timeout_s = 60);
    bool           ensure_session(std::string& error);
    void           load_session();
    void           save_session();

    // Session and helper lifetime.
    mutable std::recursive_mutex m_mutex;
    int                          m_pid{-1};
    int                          m_to_bridge{-1};
    std::thread                  m_reader;

    // Replies matched to requests by id.
    std::mutex                        m_reply_mutex;
    std::condition_variable           m_reply_cv;
    std::map<int, nlohmann::json>     m_replies;
    int                               m_next_id{1};
    bool                              m_bridge_alive{false};

    std::mutex   m_handler_mutex;
    EventHandler m_handler;

    std::atomic<bool> m_session_loaded{false};
    // Read from the UI thread without waiting for a request in flight.
    std::atomic<bool> m_logged_in{false};
    mutable std::mutex m_profile_mutex;
    std::string    m_access_token;
    std::string    m_refresh_token;
    int64_t        m_expires_at{0}; // seconds since epoch
    std::string    m_client_id;
    nlohmann::json m_profile;
    bool           m_session_started{false};
    bool           m_realtime{false};
};

} // namespace Slic3r
