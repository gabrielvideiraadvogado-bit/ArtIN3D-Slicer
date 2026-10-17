#pragma once

// ArtIN3D: the Printers tab. One sub-tab per brand, each with its own account and
// connection: Elegoo (printer manager ported from ElegooSlicer) and Flashforge.

#include <wx/notebook.h>
#include <wx/panel.h>

#include <boost/filesystem/path.hpp>

#include <string>

namespace Slic3r { namespace GUI {

class PrinterManagerView;
class FlashforgePanel;

class PrintersHubPanel : public wxPanel
{
public:
    explicit PrintersHubPanel(wxWindow* parent);

    PrinterManagerView* elegoo() const { return m_elegoo; }

    // Shows the Flashforge sub-tab with the send dialog for a sliced file.
    bool send_to_flashforge(const boost::filesystem::path& gcode, const std::string& dst_name, std::string& error);

private:
    wxNotebook*         m_book{nullptr};
    PrinterManagerView* m_elegoo{nullptr};
    FlashforgePanel*    m_flashforge{nullptr};
};

}} // namespace Slic3r::GUI
