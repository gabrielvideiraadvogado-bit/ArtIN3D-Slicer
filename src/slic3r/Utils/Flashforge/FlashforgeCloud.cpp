#include "FlashforgeCloud.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r_version.h"
#include "slic3r/GUI/GUI_App.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <wx/stdpaths.h>

#include <chrono>
#include <cstdio>
#include <cstring>

#ifdef __APPLE__
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace Slic3r {

namespace {

const char* SECTION = "flashforge_cloud";
const char* FLASH_STUDIO_LIBRARY = "/Applications/Flash Studio.app/Contents/MacOS/libFlashNetwork.dylib";
// Refresh the access token this long before it expires.
const int64_t REFRESH_MARGIN_S = 10 * 60;
// Requests to the cloud time out inside the helper after 15 s; this only guards a hung helper.
const auto REPLY_TIMEOUT = std::chrono::seconds(60);

int64_t now_s()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

boost::filesystem::path bridge_path()
{
    return boost::filesystem::path(wxStandardPaths::Get().GetExecutablePath().ToUTF8().data()).parent_path() / "artin3d-ffbridge";
}

AppConfig* app_config() { return GUI::wxGetApp().app_config; }

} // namespace

FlashforgeCloud& FlashforgeCloud::instance()
{
    static FlashforgeCloud cloud;
    return cloud;
}

FlashforgeCloud::~FlashforgeCloud() { stop_bridge(); }

bool FlashforgeCloud::available(std::string& reason)
{
#ifdef __APPLE__
    if (!boost::filesystem::exists(bridge_path())) {
        reason = "artin3d-ffbridge is missing from the application";
        return false;
    }
    if (!boost::filesystem::exists(FLASH_STUDIO_LIBRARY)) {
        reason = "Flash Studio is not installed";
        return false;
    }
    return true;
#else
    reason = "only available on macOS";
    return false;
#endif
}

void FlashforgeCloud::set_event_handler(EventHandler handler)
{
    std::lock_guard<std::mutex> lock(m_handler_mutex);
    m_handler = std::move(handler);
}

bool FlashforgeCloud::start_bridge(std::string& error)
{
#ifdef __APPLE__
    if (m_pid > 0)
        return true;
    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) {
        error = "cannot create pipes for the Flashforge helper";
        return false;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in_pipe[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, in_pipe[1]);
    posix_spawn_file_actions_addclose(&actions, out_pipe[0]);

    // The helper is x86_64 only; arch(1) starts it under Rosetta on Apple silicon.
    const std::string helper = bridge_path().string();
    char* argv[] = {const_cast<char*>("/usr/bin/arch"), const_cast<char*>("-x86_64"), const_cast<char*>(helper.c_str()), nullptr};
    pid_t pid    = -1;
    const int ret = posix_spawn(&pid, "/usr/bin/arch", &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(in_pipe[0]);
    close(out_pipe[1]);
    if (ret != 0) {
        close(in_pipe[1]);
        close(out_pipe[0]);
        error = std::string("cannot start the Flashforge helper: ") + std::strerror(ret);
        return false;
    }
    // A helper that died must not take the slicer down with SIGPIPE on the next write.
    fcntl(in_pipe[1], F_SETNOSIGPIPE, 1);
    m_pid       = pid;
    m_to_bridge = in_pipe[1];
    {
        std::lock_guard<std::mutex> lock(m_reply_mutex);
        m_bridge_alive = true;
        m_replies.clear();
    }
    FILE* stream = fdopen(out_pipe[0], "r");
    m_reader     = std::thread([this, stream]() { read_loop(stream); });

    const std::string log_dir = (boost::filesystem::path(data_dir()) / "FlashNetwork").string();
    boost::filesystem::create_directories(log_dir);
    nlohmann::json hello = call("hello", {{"logDir", log_dir}, {"userAgent", std::string("ArtIN3DSlicer/") + SLIC3R_VERSION + " (PC; macOS)"}},
                                error);
    if (!error.empty()) {
        stop_bridge();
        return false;
    }
    BOOST_LOG_TRIVIAL(info) << "Flashforge helper started, library " << hello.value("libraryVersion", "");
    return true;
#else
    error = "only available on macOS";
    return false;
#endif
}

void FlashforgeCloud::read_loop(void* stream)
{
#ifdef __APPLE__
    FILE*  in     = static_cast<FILE*>(stream);
    char*  buffer = nullptr;
    size_t size   = 0;
    while (getline(&buffer, &size, in) >= 0) {
        const nlohmann::json message = nlohmann::json::parse(buffer, nullptr, false);
        if (!message.is_object())
            continue;
        if (message.contains("event")) {
            EventHandler handler;
            {
                std::lock_guard<std::mutex> lock(m_handler_mutex);
                handler = m_handler;
            }
            if (handler)
                handler(message);
            continue;
        }
        std::lock_guard<std::mutex> lock(m_reply_mutex);
        m_replies[message.value("id", 0)] = message;
        m_reply_cv.notify_all();
    }
    free(buffer);
    fclose(in);
    std::lock_guard<std::mutex> lock(m_reply_mutex);
    m_bridge_alive = false;
    m_reply_cv.notify_all();
#endif
}

void FlashforgeCloud::stop_bridge()
{
#ifdef __APPLE__
    if (m_to_bridge >= 0)
        close(m_to_bridge); // EOF makes the helper exit, which ends read_loop
    if (m_pid > 0) {
        int status = 0;
        for (int i = 0; i < 20 && waitpid(m_pid, &status, WNOHANG) == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (waitpid(m_pid, &status, WNOHANG) == 0) {
            kill(m_pid, SIGTERM);
            waitpid(m_pid, &status, 0);
        }
    }
    if (m_reader.joinable())
        m_reader.join();
#endif
    m_pid       = -1;
    m_to_bridge = -1;
    m_realtime  = false;
}

nlohmann::json FlashforgeCloud::call(const std::string& method, const nlohmann::json& params, std::string& error)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    error.clear();
    if (method != "hello" && m_pid <= 0 && !start_bridge(error))
        return nullptr;
#ifdef __APPLE__
    int id;
    {
        std::lock_guard<std::mutex> reply_lock(m_reply_mutex);
        id = m_next_id++;
    }
    const std::string line = nlohmann::json{{"id", id}, {"method", method}, {"params", params}}.dump() + "\n";
    if (write(m_to_bridge, line.data(), line.size()) != static_cast<ssize_t>(line.size())) {
        error = "the Flashforge helper stopped";
        stop_bridge();
        return nullptr;
    }
    nlohmann::json reply;
    {
        std::unique_lock<std::mutex> reply_lock(m_reply_mutex);
        const bool answered = m_reply_cv.wait_for(reply_lock, REPLY_TIMEOUT,
                                                  [&]() { return m_replies.count(id) > 0 || !m_bridge_alive; });
        if (!answered || m_replies.count(id) == 0) {
            error = "the Flashforge helper did not answer";
            reply_lock.unlock();
            stop_bridge();
            return nullptr;
        }
        reply = std::move(m_replies[id]);
        m_replies.erase(id);
    }
    if (!reply.value("ok", false)) {
        error = reply.value("error", std::string("Flashforge request failed"));
        return nullptr;
    }
    return reply.value("result", nlohmann::json());
#else
    error = "only available on macOS";
    return nullptr;
#endif
}

void FlashforgeCloud::load_session()
{
    if (m_session_loaded)
        return;
    m_session_loaded = true;
    AppConfig* config = app_config();
    if (config == nullptr)
        return;
    m_access_token  = config->get(SECTION, "access_token");
    m_refresh_token = config->get(SECTION, "refresh_token");
    m_client_id     = config->get(SECTION, "client_id");
    const std::string expires = config->get(SECTION, "expires_at");
    try {
        m_expires_at = expires.empty() ? 0 : std::stoll(expires);
    } catch (const std::exception&) {
        m_expires_at = 0; // forces a token refresh
    }
    m_logged_in = !m_refresh_token.empty();
}

void FlashforgeCloud::save_session()
{
    AppConfig* config = app_config();
    if (config == nullptr)
        return;
    config->set(SECTION, "access_token", m_access_token);
    config->set(SECTION, "refresh_token", m_refresh_token);
    config->set(SECTION, "client_id", m_client_id);
    config->set(SECTION, "expires_at", std::to_string(m_expires_at));
    m_logged_in = !m_refresh_token.empty();
}

bool FlashforgeCloud::logged_in() const
{
    if (!m_session_loaded) {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        const_cast<FlashforgeCloud*>(this)->load_session();
    }
    return m_logged_in;
}

nlohmann::json FlashforgeCloud::profile() const
{
    std::lock_guard<std::mutex> lock(m_profile_mutex);
    return m_profile;
}

bool FlashforgeCloud::login_with_token(const std::string& access_token, const std::string& refresh_token, int64_t expires_in,
                                       int64_t start_time, std::string& error)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    load_session();
    // The web page sends start_time in milliseconds or seconds depending on the flow.
    const int64_t start = start_time > 100000000000LL ? start_time / 1000 : (start_time > 0 ? start_time : now_s());
    m_access_token      = access_token;
    m_refresh_token     = refresh_token;
    m_expires_at        = start + expires_in;
    m_session_started   = false;
    save_session();
    return ensure_session(error);
}

void FlashforgeCloud::logout()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    load_session();
    std::string ignored;
    if (m_realtime)
        call("disconnect", nlohmann::json::object(), ignored);
    if (!m_access_token.empty())
        call("signOut", {{"accessToken", m_access_token}}, ignored);
    m_access_token.clear();
    m_refresh_token.clear();
    m_expires_at      = 0;
    {
        std::lock_guard<std::mutex> profile_lock(m_profile_mutex);
        m_profile = nullptr;
    }
    m_session_started = false;
    m_realtime        = false;
    save_session();
}

bool FlashforgeCloud::ensure_session(std::string& error)
{
    load_session();
    if (m_refresh_token.empty()) {
        error = "not logged in";
        return false;
    }
    if (m_access_token.empty() || now_s() > m_expires_at - REFRESH_MARGIN_S) {
        nlohmann::json token = call("refreshToken", {{"refreshToken", m_refresh_token}}, error);
        if (!error.empty()) {
            BOOST_LOG_TRIVIAL(warning) << "Flashforge token refresh failed: " << error;
            return false;
        }
        m_access_token    = token.value("accessToken", std::string());
        m_refresh_token   = token.value("refreshToken", m_refresh_token);
        m_expires_at      = now_s() + token.value("expiresIn", 0);
        m_session_started = false;
        save_session();
        if (m_realtime) {
            std::string ignored;
            call("setToken", {{"accessToken", m_access_token}}, ignored);
        }
    }
    if (!m_session_started) {
        nlohmann::json session = call("startSession", {{"accessToken", m_access_token}, {"clientId", m_client_id}}, error);
        if (!error.empty())
            return false;
        m_client_id       = session.value("clientId", m_client_id);
        {
            std::lock_guard<std::mutex> profile_lock(m_profile_mutex);
            m_profile = session.value("profile", nlohmann::json::object());
        }
        m_session_started = true;
        save_session();
    }
    return true;
}

nlohmann::json FlashforgeCloud::printers_with_detail(std::string& error)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    nlohmann::json result = nlohmann::json::array();
    if (!ensure_session(error))
        return result;
    nlohmann::json list = call("wanDevList", {{"clientId", m_client_id}, {"accessToken", m_access_token}}, error);
    if (!error.empty())
        return result;
    for (nlohmann::json printer : list) {
        std::string    detail_error;
        nlohmann::json detail = call("wanDevDetail",
                                     {{"clientId", m_client_id}, {"accessToken", m_access_token}, {"devId", printer.value("devId", "")}},
                                     detail_error);
        if (detail_error.empty())
            printer["detail"] = detail;
        else
            printer["detailError"] = detail_error;
        result.push_back(printer);
    }
    return result;
}

bool FlashforgeCloud::connect_realtime(const std::vector<std::string>& dev_topics, std::string& error)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!ensure_session(error))
        return false;
    call("connect", {{"clientId", m_client_id}, {"accessToken", m_access_token}, {"devTopics", dev_topics}}, error);
    m_realtime = error.empty();
    return m_realtime;
}

bool FlashforgeCloud::request_detail(std::string& error)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_realtime) {
        error = "not connected";
        return false;
    }
    call("updateDetail", nlohmann::json::object(), error);
    return error.empty();
}

bool FlashforgeCloud::command(const std::string& dev_topic, const std::string& action, const nlohmann::json& args, std::string& error)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_realtime) {
        error = "not connected";
        return false;
    }
    nlohmann::json params = args.is_object() ? args : nlohmann::json::object();
    params["devTopic"]    = dev_topic;
    params["action"]      = action;
    call("command", params, error);
    return error.empty();
}

} // namespace Slic3r
