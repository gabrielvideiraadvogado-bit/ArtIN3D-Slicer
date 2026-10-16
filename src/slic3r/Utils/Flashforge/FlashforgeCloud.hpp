#pragma once

// ArtIN3D: Flashforge account (cloud) access through the artin3d-ffbridge helper.
//
// Flashforge's network library ships only as a closed x86_64 binary inside Flash Studio,
// so it runs in a separate helper process under Rosetta (see src/artin3d_ffbridge). This
// class owns that process and the account session (tokens in AppConfig section
// "flashforge_cloud"). Every call blocks: run it off the UI thread.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <mutex>
#include <string>

namespace Slic3r {

class FlashforgeCloud
{
public:
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

private:
    FlashforgeCloud() = default;
    ~FlashforgeCloud();
    FlashforgeCloud(const FlashforgeCloud&) = delete;
    FlashforgeCloud& operator=(const FlashforgeCloud&) = delete;

    bool           start_bridge(std::string& error);
    void           stop_bridge();
    nlohmann::json call(const std::string& method, const nlohmann::json& params, std::string& error);
    bool           ensure_session(std::string& error);
    void           load_session();
    void           save_session();

    mutable std::recursive_mutex m_mutex;
    int                          m_pid{-1};
    int                          m_to_bridge{-1};
    void*                        m_from_bridge{nullptr}; // FILE*
    int                          m_next_id{1};
    bool                         m_session_loaded{false};

    std::string    m_access_token;
    std::string    m_refresh_token;
    int64_t        m_expires_at{0}; // seconds since epoch
    std::string    m_client_id;
    nlohmann::json m_profile;
    bool           m_session_started{false};
};

} // namespace Slic3r
