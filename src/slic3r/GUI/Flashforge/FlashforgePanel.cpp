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

namespace Slic3r { namespace GUI {

namespace {

const char* LOGIN_URL = "https://desktop.voxelshare.com";
// The printers' state is polled while the panel is visible.
const int REFRESH_INTERVAL_MS = 10000;

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
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { refresh_printers(); }, m_refresh_timer.GetId());
}

FlashforgePanel::~FlashforgePanel()
{
    *m_alive = false;
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
        FlashforgeCloud::instance().logout();
        send_account_state();
        send_to_page("printers", {{"printers", nlohmann::json::array()}, {"error", ""}});
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
