#include "PrintersHubPanel.hpp"

#include "Elegoo/PrinterManagerView.hpp"
#include "Flashforge/FlashforgePanel.hpp"
#include "GUI_App.hpp"

#include <wx/sizer.h>

namespace Slic3r { namespace GUI {

PrintersHubPanel::PrintersHubPanel(wxWindow* parent) : wxPanel(parent, wxID_ANY)
{
    m_book       = new wxNotebook(this, wxID_ANY);
    m_elegoo     = new PrinterManagerView(m_book);
    m_flashforge = new FlashforgePanel(m_book);
    m_book->AddPage(m_elegoo, "Elegoo", true);
    m_book->AddPage(m_flashforge, "Flashforge");

    // Pages only poll their printers while visible.
    m_book->Bind(wxEVT_NOTEBOOK_PAGE_CHANGED, [this](wxBookCtrlEvent& evt) {
        for (size_t i = 0; i < m_book->GetPageCount(); ++i)
            m_book->GetPage(i)->Show(static_cast<int>(i) == evt.GetSelection());
        evt.Skip();
    });

    // The Elegoo web view is created once the window exists, as ElegooSlicer does.
    m_elegoo->CallAfter([view = m_elegoo]() {
        if (!wxGetApp().is_closing() && !view->IsBeingDeleted())
            view->initializeWebView();
    });

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(m_book, 1, wxEXPAND);
    SetSizer(sizer);
}

bool PrintersHubPanel::send_to_flashforge(const boost::filesystem::path& gcode, const std::string& dst_name, std::string& error)
{
    const int page = m_book->FindPage(m_flashforge);
    if (page != wxNOT_FOUND && m_book->GetSelection() != page)
        m_book->SetSelection(page);
    return m_flashforge->offer_file(gcode, dst_name, error);
}

}} // namespace Slic3r::GUI
