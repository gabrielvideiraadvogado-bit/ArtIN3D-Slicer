#include "FlashforgePanel.hpp"

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Widgets/WebView.hpp"
#include "slic3r/Utils/Flashforge/FlashforgeCloud.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <wx/sizer.h>

#include <thread>
#include <vector>

namespace Slic3r { namespace GUI {

namespace {

const char* LOGIN_URL = "https://desktop.voxelshare.com";
// While the panel is visible: without the real-time connection the printers are polled;
// with it they push their state, and the timer only asks for a fresh full detail.
const int REFRESH_INTERVAL_MS = 15000;

} // namespace

FlashforgePanel::FlashforgePanel(wxWindow* parent) : wxPanel(parent, wxID_ANY)
{
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    m_browser   = WebView::CreateWebView(this, "");
    if (m_browser == nullptr) {
        BOOST_LOG_TRIVIAL(error) << "FlashforgePanel: cannot create the web view";
        SetSizer(sizer);
        return;
    }
    m_browser->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &FlashforgePanel::on_script_message, this);
    sizer->Add(m_browser, 1, wxEXPAND);
    SetSizer(sizer);

    const boost::filesystem::path page = boost::filesystem::path(resources_dir()) / "web" / "artin3d" / "flashforge" / "index.html";
    WebView::LoadUrl(m_browser, file_url_from_path(page));

    m_refresh_timer.SetOwner(this);
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
        if (!m_realtime) {
            refresh_printers();
            return;
        }
        std::thread([]() {
            std::string ignored;
            FlashforgeCloud::instance().request_detail(ignored);
        }).detach();
    }, m_refresh_timer.GetId());

    std::weak_ptr<bool> alive = m_alive;
    FlashforgeCloud::instance().set_event_handler([this, alive](const nlohmann::json& event) {
        wxGetApp().CallAfter([this, alive, event]() {
            if (!alive.expired() && *alive.lock())
                on_cloud_event(event);
        });
    });
}

FlashforgePanel::~FlashforgePanel()
{
    *m_alive = false;
    FlashforgeCloud::instance().set_event_handler(nullptr);
    m_refresh_timer.Stop();
}

bool FlashforgePanel::Show(bool show)
{
    if (show && m_page_ready && FlashforgeCloud::instance().logged_in()) {
        refresh_printers();
        m_refresh_timer.Start(REFRESH_INTERVAL_MS);
    } else if (!show) {
        m_refresh_timer.Stop();
    }
    return wxPanel::Show(show);
}

void FlashforgePanel::send_to_page(const std::string& type, const nlohmann::json& data)
{
    if (m_browser == nullptr)
        return;
    const nlohmann::json message = {{"type", type}, {"data", data}};
    WebView::RunScript(m_browser, wxString::FromUTF8("window.artin3dFlashforge && window.artin3dFlashforge.receive(" + message.dump() + ");"));
}

void FlashforgePanel::send_account_state()
{
    std::string    reason;
    const bool     available = FlashforgeCloud::instance().available(reason);
    nlohmann::json state     = {{"available", available}, {"reason", reason},
                                {"loggedIn", available && FlashforgeCloud::instance().logged_in()},
                                {"profile", FlashforgeCloud::instance().profile()}};
    send_to_page("account", state);
}

void FlashforgePanel::refresh_printers()
{
    if (!FlashforgeCloud::instance().logged_in() || m_refreshing.exchange(true))
        return;
    send_to_page("loading", true);
    std::weak_ptr<bool> alive = m_alive;
    std::thread([this, alive]() {
        std::string    error;
        nlohmann::json printers = FlashforgeCloud::instance().printers_with_detail(error);
        wxGetApp().CallAfter([this, alive, printers, error]() {
            if (alive.expired() || !*alive.lock())
                return;
            m_refreshing = false;
            send_account_state();
            send_to_page("printers", {{"printers", printers}, {"error", error}});
            if (error.empty())
                connect_realtime(printers);
        });
    }).detach();
}

void FlashforgePanel::connect_realtime(const nlohmann::json& printers)
{
    std::vector<std::string> topics;
    m_dev_topics.clear();
    for (const nlohmann::json& printer : printers) {
        const std::string topic = printer.value("devTopic", std::string());
        m_dev_topics[printer.value("devId", std::string())] = topic;
        if (!topic.empty())
            topics.push_back(topic);
    }
    if (m_realtime || topics.empty())
        return;
    m_realtime = true;
    std::weak_ptr<bool> alive = m_alive;
    std::thread([this, alive, topics]() {
        std::string error;
        const bool  ok = FlashforgeCloud::instance().connect_realtime(topics, error);
        wxGetApp().CallAfter([this, alive, ok, error]() {
            if (alive.expired() || !*alive.lock())
                return;
            m_realtime = ok;
            if (ok)
                return; // the helper reports the connected state itself
            BOOST_LOG_TRIVIAL(warning) << "Flashforge real-time connection failed: " << error;
            send_to_page("connection", {{"connected", false}, {"error", error}});
        });
    }).detach();
}

void FlashforgePanel::on_cloud_event(const nlohmann::json& event)
{
    const std::string type = event.value("event", std::string());
    if (type == "detail")
        send_to_page("detail", event.value("data", nlohmann::json::object()));
    else if (type == "connection")
        send_to_page("connection", {{"connected", event.value("connected", false)}});
    else if (type == "offline" || type == "online")
        send_to_page(type, {{"devId", event.value("devId", std::string())}});
    else if (type == "devicesChanged")
        refresh_printers();
}

void FlashforgePanel::run_command(const std::string& dev_id, const std::string& action, const nlohmann::json& args)
{
    const auto it = m_dev_topics.find(dev_id);
    if (it == m_dev_topics.end() || it->second.empty()) {
        send_to_page("commandResult", {{"devId", dev_id}, {"action", action}, {"error", "printer not found"}});
        return;
    }
    const std::string   topic = it->second;
    std::weak_ptr<bool> alive = m_alive;
    std::thread([this, alive, topic, dev_id, action, args]() {
        std::string error;
        FlashforgeCloud::instance().command(topic, action, args, error);
        // Ask for the new state right away instead of waiting for the next push.
        std::string ignored;
        if (error.empty())
            FlashforgeCloud::instance().request_detail(ignored);
        wxGetApp().CallAfter([this, alive, dev_id, action, error]() {
            if (!alive.expired() && *alive.lock())
                send_to_page("commandResult", {{"devId", dev_id}, {"action", action}, {"error", error}});
        });
    }).detach();
}

void FlashforgePanel::open_login()
{
    FlashforgeLoginDialog dialog(this);
    if (dialog.ShowModal() != wxID_OK)
        return;
    const nlohmann::json token = dialog.token();
    send_to_page("loading", true);
    std::weak_ptr<bool> alive = m_alive;
    std::thread([this, alive, token]() {
        std::string error;
        // The page sends these as numbers or numeric strings depending on the sign-in flow.
        auto as_int = [&token](const char* key) -> int64_t {
            const auto it = token.find(key);
            if (it == token.end())
                return 0;
            if (it->is_number())
                return it->get<int64_t>();
            try {
                return it->is_string() ? std::stoll(it->get<std::string>()) : 0;
            } catch (const std::exception&) {
                return 0;
            }
        };
        FlashforgeCloud::instance().login_with_token(token.value("access_token", std::string()),
                                                     token.value("refresh_token", std::string()), as_int("expires_in"),
                                                     as_int("start_time"), error);
        wxGetApp().CallAfter([this, alive, error]() {
            if (alive.expired() || !*alive.lock())
                return;
            send_account_state();
            if (!error.empty()) {
                send_to_page("printers", {{"printers", nlohmann::json::array()}, {"error", error}});
                return;
            }
            refresh_printers();
            m_refresh_timer.Start(REFRESH_INTERVAL_MS);
        });
    }).detach();
}

void FlashforgePanel::on_script_message(wxWebViewEvent& event)
{
    const nlohmann::json message = nlohmann::json::parse(event.GetString().ToUTF8().data(), nullptr, false);
    if (!message.is_object())
        return;
    const std::string command = message.value("command", std::string());
    if (command == "ready") {
        m_page_ready = true;
        send_account_state();
        if (FlashforgeCloud::instance().logged_in() && IsShownOnScreen()) {
            refresh_printers();
            m_refresh_timer.Start(REFRESH_INTERVAL_MS);
        }
    } else if (command == "login") {
        open_login();
    } else if (command == "logout") {
        m_refresh_timer.Stop();
        m_realtime = false;
        m_dev_topics.clear();
        std::weak_ptr<bool> alive = m_alive;
        std::thread([this, alive]() {
            FlashforgeCloud::instance().logout();
            wxGetApp().CallAfter([this, alive]() {
                if (alive.expired() || !*alive.lock())
                    return;
                send_account_state();
                send_to_page("printers", {{"printers", nlohmann::json::array()}, {"error", ""}});
            });
        }).detach();
    } else if (command == "light" || command == "camera" || command == "loadFilament" || command == "unloadFilament") {
        const nlohmann::json data = message.value("data", nlohmann::json::object());
        run_command(data.value("devId", std::string()), command, data);
    } else if (command == "refresh") {
        refresh_printers();
    } else if (command == "openFlashStudio") {
        wxLaunchDefaultApplication("/Applications/Flash Studio.app");
    }
}

FlashforgeLoginDialog::FlashforgeLoginDialog(wxWindow* parent)
    : wxDialog(parent, wxID_ANY, _L("Flashforge account"), wxDefaultPosition, wxSize(900, 720),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    m_browser   = WebView::CreateWebView(this, "");
    if (m_browser != nullptr) {
        m_browser->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &FlashforgeLoginDialog::on_script_message, this);
        // Apple/Google/Facebook sign-in must come back to this page, so popups stay here.
        m_browser->Bind(wxEVT_WEBVIEW_NEWWINDOW, [this](wxWebViewEvent& evt) {
            if (!evt.GetURL().empty())
                m_browser->LoadURL(evt.GetURL());
            evt.Veto();
        });
        sizer->Add(m_browser, 1, wxEXPAND);
        WebView::LoadUrl(m_browser, LOGIN_URL);
    }
    SetSizer(sizer);
    CenterOnParent();
}

void FlashforgeLoginDialog::on_script_message(wxWebViewEvent& event)
{
    const nlohmann::json message = nlohmann::json::parse(event.GetString().ToUTF8().data(), nullptr, false);
    if (!message.is_object() || message.value("command", std::string()) != "homepage_get_token")
        return;
    const nlohmann::json data = message.value("data", nlohmann::json::object());
    if (data.value("access_token", std::string()).empty() || data.value("refresh_token", std::string()).empty())
        return;
    m_token = data;
    EndModal(wxID_OK);
}

}} // namespace Slic3r::GUI
