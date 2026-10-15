#pragma once
#include <map>
#include <thread>
#include <mutex>
#include <atomic>
#include "slic3r/Utils/Singleton.hpp"
#include "libslic3r/PrinterNetworkResult.hpp"
#include "libslic3r/PrinterNetworkInfo.hpp"

namespace Slic3r { 

class PrinterPluginManager : public Singleton<PrinterPluginManager>
{
    friend class Singleton<PrinterPluginManager>;
public:
    ~PrinterPluginManager();
    PrinterPluginManager(const PrinterPluginManager&) = delete;
    PrinterPluginManager& operator=(const PrinterPluginManager&) = delete;

    PrinterNetworkResult<PluginNetworkInfo> hasInstalledPlugin(const std::string& hostType);
    PrinterNetworkResult<bool> installPlugin(const PluginNetworkInfo& plugin);
    PrinterNetworkResult<bool> uninstallPlugin(const PluginNetworkInfo& plugin);

    std::vector<std::string> getSupportedPluginTypeList();
    
    bool init();
    bool uninit();

private:
    PrinterPluginManager();

    std::mutex mPluginsMutex;
    std::map<std::string, PluginNetworkInfo> mPluginList;

    void loadPluginList();

    void deletePlugin(const std::string& hostType);
    void addPlugin(const PluginNetworkInfo& plugin);
    void updatePlugin(const PluginNetworkInfo& plugin);
    PluginNetworkInfo getPlugin(const std::string& hostType);

    std::mutex mInitMutex;
    bool mInitialized{false};
};
} // namespace Slic3r 
