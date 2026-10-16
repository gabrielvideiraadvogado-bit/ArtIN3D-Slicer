#pragma once

// ArtIN3D: Flashforge printers of the user's Flashforge account (cloud), shown in the
// Printers tab. The page is resources/web/artin3d/flashforge/index.html; this panel feeds
// it and runs the account requests (FlashforgeCloud) on a worker thread.

#include <nlohmann/json.hpp>
#include <wx/dialog.h>
#include <wx/panel.h>
#include <wx/timer.h>
#include <wx/webview.h>

#include <atomic>

namespace Slic3r { namespace GUI {

class FlashforgePanel : public wxPanel
{
public:
    explicit FlashforgePanel(wxWindow* parent);
    ~FlashforgePanel() override;

    bool Show(bool show = true) override;

private:
    void on_script_message(wxWebViewEvent& event);
    void send_to_page(const std::string& type, const nlohmann::json& data);
    void send_account_state();
    void refresh_printers();
    void open_login();

    wxWebView*        m_browser{nullptr};
    wxTimer           m_refresh_timer;
    std::atomic<bool> m_refreshing{false};
    std::atomic<bool> m_page_ready{false};
    std::shared_ptr<bool> m_alive{std::make_shared<bool>(true)};
};

// Flashforge's own web login (desktop.voxelshare.com), which offers e-mail, Apple, Google
// and Facebook sign-in and hands the session to the app through window.wx.postMessage.
class FlashforgeLoginDialog : public wxDialog
{
public:
    explicit FlashforgeLoginDialog(wxWindow* parent);

    // Valid after ShowModal() returned wxID_OK.
    const nlohmann::json& token() const { return m_token; }

private:
    void on_script_message(wxWebViewEvent& event);

    wxWebView*     m_browser{nullptr};
    nlohmann::json m_token;
};

}} // namespace Slic3r::GUI
