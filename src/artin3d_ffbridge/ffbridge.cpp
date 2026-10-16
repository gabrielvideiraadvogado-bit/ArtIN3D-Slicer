// ArtIN3D Flashforge bridge.
//
// Flashforge only ships its network library (libFlashNetwork, used by Flash Studio for
// account login, cloud printers, camera and controls) as a closed x86_64 binary, which a
// native arm64 process cannot load. This small helper runs under Rosetta, loads the copy
// installed with Flash Studio (it is never redistributed) and serves the slicer over
// stdin/stdout, one JSON object per line:
//
//   request:  {"id": 1, "method": "wanDevList", "params": {...}}
//   reply:    {"id": 1, "ok": true, "result": ...}  or  {"id": 1, "ok": false, "code": n, "error": "..."}
//
// The function declarations come from FlashNetwork.h of FlashForge/Orca-Flashforge 1.7.15
// (AGPL-3.0), which matches library version 3.4.x. Passwords are passed through to the
// library and never stored or logged here.

#include "FlashNetwork.h"

#include <nlohmann/json.hpp>

#include <dlfcn.h>
#include <iostream>
#include <random>
#include <string>

using json = nlohmann::json;

namespace {

const char* DEFAULT_LIBRARY = "/Applications/Flash Studio.app/Contents/MacOS/libFlashNetwork.dylib";
const char* DEFAULT_SERVER_SETTINGS = "/Applications/Flash Studio.app/Contents/Resources/data/" DAT_FILE_NAME;
const char* SUPPORTED_VERSION_PREFIX = "3.4.";
// appId/platId Flash Studio sends from macOS for the production servers (MultiComMgr.cpp).
const long long APP_ID = 31;
const long long PLAT_ID = 15;
const int TIMEOUT_MS = 15000;

struct Lib
{
#define FNET_FUNC(name) decltype(&fnet_##name) name = nullptr;
    FNET_FUNC(initlize)
    FNET_FUNC(uninitlize)
    FNET_FUNC(getVersion)
    FNET_FUNC(setUserAgent)
    FNET_FUNC(freeString)
    FNET_FUNC(getTokenByPassword)
    FNET_FUNC(refreshToken)
    FNET_FUNC(freeToken)
    FNET_FUNC(sendSMSCode)
    FNET_FUNC(getTokenBySMSCode)
    FNET_FUNC(signOut)
    FNET_FUNC(getUserProfile)
    FNET_FUNC(freeUserProfile)
    FNET_FUNC(bindAccountRelp)
    FNET_FUNC(freeBindAccountRelpResult)
    FNET_FUNC(getWanDevList)
    FNET_FUNC(freeWanDevList)
    FNET_FUNC(getWanDevProductDetail)
    FNET_FUNC(freeDevProduct)
    FNET_FUNC(freeDevDetail)
    FNET_FUNC(getLanDevList)
    FNET_FUNC(freeLanDevInfos)
    FNET_FUNC(getLanDevDetail)
    FNET_FUNC(ctrlLanDevLight)
#undef FNET_FUNC
    std::string version;
    bool        initialized = false;
};

Lib g_lib;

std::string str(const char* s) { return s ? std::string(s) : std::string(); }

struct BridgeError
{
    int         code;
    std::string message;
};

// Takes ownership of a message string returned by the library.
std::string take_message(char* message)
{
    std::string text = str(message);
    if (message != nullptr && g_lib.freeString != nullptr)
        g_lib.freeString(message);
    return text;
}

void check(int ret, const std::string& what, const std::string& message = {})
{
    if (ret != FNET_OK)
        throw BridgeError{ret, message.empty() ? what + " failed" : message};
}

const json& require(const json& params, const char* key)
{
    if (!params.contains(key) || params[key].is_null())
        throw BridgeError{FNET_ERROR, std::string("missing parameter: ") + key};
    return params[key];
}

std::string param_str(const json& params, const char* key) { return require(params, key).get<std::string>(); }

json token_json(const fnet_token_data_t* token)
{
    return {{"accessToken", str(token->accessToken)}, {"refreshToken", str(token->refreshToken)}, {"expiresIn", token->expiresIn}};
}

json detail_json(const fnet_dev_detail_t* d)
{
    json slots = json::array();
    if (d->hasMatlStation && d->matlStationInfo.slotInfos != nullptr) {
        for (int i = 0; i < d->matlStationInfo.slotCnt; ++i) {
            const fnet_matl_slot_info_t& s = d->matlStationInfo.slotInfos[i];
            slots.push_back({{"slotId", s.slotId}, {"hasFilament", s.hasFilament != 0},
                             {"material", str(s.materialName)}, {"color", str(s.materialColor)}});
        }
    }
    return {
        {"devId", str(d->devId)},
        {"name", str(d->name)},
        {"status", str(d->status)},
        {"firmwareVersion", str(d->firmwareVersion)},
        {"ipAddr", str(d->ipAddr)},
        {"printFileName", str(d->printFileName)},
        {"printFileThumbUrl", str(d->printFileThumbUrl)},
        {"printProgress", d->printProgress},
        {"printLayer", d->printLayer},
        {"targetPrintLayer", d->targetPrintLayer},
        {"printDuration", d->printDuration},
        {"estimatedTime", d->estimatedTime},
        {"nozzleTemp", d->rightTemp},
        {"nozzleTargetTemp", d->rightTargetTemp},
        {"bedTemp", d->platTemp},
        {"bedTargetTemp", d->platTargetTemp},
        {"chamberTemp", d->chamberTemp},
        {"lightStatus", str(d->lightStatus)},
        {"doorStatus", str(d->doorStatus)},
        {"camera", d->camera},
        {"cameraStreamUrl", str(d->cameraStreamUrl)},
        {"errorCode", str(d->errorCode)},
        {"hasMatlStation", d->hasMatlStation != 0},
        {"matlStation", {{"currentSlot", d->matlStationInfo.currentSlot},
                         {"currentLoadSlot", d->matlStationInfo.currentLoadSlot},
                         {"stateAction", d->matlStationInfo.stateAction},
                         {"stateStep", d->matlStationInfo.stateStep},
                         {"slots", slots}}},
    };
}

std::string random_client_id()
{
    std::random_device rd;
    std::mt19937_64    gen(rd());
    const char*        hex = "0123456789abcdef";
    std::string        id  = "pc_artin3d_";
    for (int i = 0; i < 32; ++i)
        id += hex[gen() % 16];
    return id;
}

json hello(const json& params)
{
    const std::string library  = params.value("library", std::string(DEFAULT_LIBRARY));
    const std::string settings = params.value("serverSettings", std::string(DEFAULT_SERVER_SETTINGS));
    const std::string logDir   = params.value("logDir", std::string());

    if (!g_lib.initialized) {
        void* handle = dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (handle == nullptr)
            throw BridgeError{FNET_ERROR, std::string("cannot load Flashforge library: ") + dlerror()};
#define LOAD(name)                                                                        \
    g_lib.name = reinterpret_cast<decltype(g_lib.name)>(dlsym(handle, "fnet_" #name));    \
    if (g_lib.name == nullptr)                                                            \
        throw BridgeError{FNET_ERROR, "Flashforge library lacks fnet_" #name};
        LOAD(initlize) LOAD(uninitlize) LOAD(getVersion) LOAD(setUserAgent) LOAD(freeString)
        LOAD(getTokenByPassword) LOAD(refreshToken) LOAD(freeToken) LOAD(sendSMSCode) LOAD(getTokenBySMSCode)
        LOAD(signOut) LOAD(getUserProfile) LOAD(freeUserProfile) LOAD(bindAccountRelp) LOAD(freeBindAccountRelpResult)
        LOAD(getWanDevList) LOAD(freeWanDevList) LOAD(getWanDevProductDetail) LOAD(freeDevProduct) LOAD(freeDevDetail)
        LOAD(getLanDevList) LOAD(freeLanDevInfos) LOAD(getLanDevDetail) LOAD(ctrlLanDevLight)
#undef LOAD
        g_lib.version = str(g_lib.getVersion());
        if (g_lib.version.rfind(SUPPORTED_VERSION_PREFIX, 0) != 0)
            throw BridgeError{FNET_ERROR, "unsupported Flashforge library version " + g_lib.version};
        fnet_log_settings_t log{};
        log.fileDir     = logDir.c_str();
        log.expireHours = 7 * 24;
        log.level       = FNET_LOG_LEVEL_INFO;
        check(g_lib.initlize(settings.c_str(), &log), "fnet_initlize");
        g_lib.setUserAgent(params.value("userAgent", std::string("ArtIN3DSlicer (PC; macOS)")).c_str());
        g_lib.initialized = true;
    }
    return {{"libraryVersion", g_lib.version}};
}

json login_password(const json& params)
{
    fnet_token_data_t* token   = nullptr;
    char*              message = nullptr;
    int ret = g_lib.getTokenByPassword(param_str(params, "user").c_str(), param_str(params, "password").c_str(),
                                       params.value("language", std::string("en")).c_str(), &token, &message, TIMEOUT_MS);
    const std::string text = take_message(message);
    check(ret, "login", text);
    json result = token_json(token);
    g_lib.freeToken(token);
    return result;
}

json send_sms(const json& params)
{
    char* message = nullptr;
    int   ret     = g_lib.sendSMSCode(param_str(params, "phone").c_str(), params.value("language", std::string("en")).c_str(),
                                      &message, TIMEOUT_MS);
    const std::string text = take_message(message);
    check(ret, "send SMS code", text);
    return json::object();
}

json login_sms(const json& params)
{
    fnet_token_data_t* token   = nullptr;
    char*              message = nullptr;
    int ret = g_lib.getTokenBySMSCode(param_str(params, "phone").c_str(), param_str(params, "code").c_str(),
                                      params.value("language", std::string("en")).c_str(), &token, &message, TIMEOUT_MS);
    const std::string text = take_message(message);
    check(ret, "login", text);
    json result = token_json(token);
    g_lib.freeToken(token);
    return result;
}

json refresh_token(const json& params)
{
    fnet_token_data_t* token   = nullptr;
    char*              message = nullptr;
    int ret = g_lib.refreshToken(param_str(params, "refreshToken").c_str(), params.value("language", std::string("en")).c_str(),
                                 &token, &message, TIMEOUT_MS);
    const std::string text = take_message(message);
    check(ret, "refresh token", text);
    json result = token_json(token);
    g_lib.freeToken(token);
    return result;
}

// Profile plus the account/client binding Flash Studio performs right after logging in;
// the cloud only lists printers to a client bound this way.
json start_session(const json& params)
{
    const std::string accessToken = param_str(params, "accessToken");
    std::string       clientId    = params.value("clientId", std::string());
    if (clientId.empty())
        clientId = random_client_id();

    fnet_user_profile_t* profile = nullptr;
    check(g_lib.getUserProfile(accessToken.c_str(), &profile, TIMEOUT_MS), "get user profile");
    json result = {{"clientId", clientId},
                   {"profile", {{"uid", str(profile->uid)}, {"nickname", str(profile->nickname)},
                                {"email", str(profile->email)}, {"avatarUrl", str(profile->headImgUrl)}}}};
    const std::string email = str(profile->email);
    g_lib.freeUserProfile(profile);

    fnet_bind_account_relp_result_t* bind = nullptr;
    check(g_lib.bindAccountRelp(clientId.c_str(), accessToken.c_str(), email.c_str(), &bind, TIMEOUT_MS), "bind account");
    g_lib.freeBindAccountRelpResult(bind);
    return result;
}

json sign_out(const json& params)
{
    check(g_lib.signOut(param_str(params, "accessToken").c_str(), TIMEOUT_MS), "sign out");
    return json::object();
}

json wan_dev_list(const json& params)
{
    fnet_wan_dev_info_t* infos = nullptr;
    int                  count = 0;
    check(g_lib.getWanDevList(param_str(params, "clientId").c_str(), param_str(params, "accessToken").c_str(), APP_ID, PLAT_ID,
                              &infos, &count, TIMEOUT_MS),
          "get cloud printers");
    json list = json::array();
    for (int i = 0; i < count; ++i) {
        const fnet_wan_dev_info_t& d = infos[i];
        list.push_back({{"devId", str(d.devId)}, {"name", str(d.name)}, {"model", str(d.model)},
                        {"imageUrl", str(d.imageUrl)}, {"status", str(d.status)},
                        {"serialNumber", str(d.serialNumber)}, {"location", str(d.location)}});
    }
    g_lib.freeWanDevList(infos, count);
    return list;
}

json wan_dev_detail(const json& params)
{
    fnet_dev_product_t* product = nullptr;
    fnet_dev_detail_t*  detail  = nullptr;
    check(g_lib.getWanDevProductDetail(param_str(params, "clientId").c_str(), param_str(params, "accessToken").c_str(),
                                       param_str(params, "devId").c_str(), &product, &detail, TIMEOUT_MS),
          "get cloud printer status");
    json result = detail_json(detail);
    g_lib.freeDevProduct(product);
    g_lib.freeDevDetail(detail);
    return result;
}

json lan_dev_list(const json& params)
{
    fnet_lan_dev_info_t* infos = nullptr;
    int                  count = 0;
    check(g_lib.getLanDevList(&infos, &count, params.value("waitMs", 3000)), "search local network");
    json list = json::array();
    for (int i = 0; i < count; ++i) {
        const fnet_lan_dev_info_t& d = infos[i];
        list.push_back({{"serialNumber", d.serialNumber}, {"name", d.name}, {"ip", d.ip}, {"port", d.port},
                        {"pid", d.pid}, {"wanMode", d.connectMode == 1}, {"bound", d.bindStatus == 1}});
    }
    g_lib.freeLanDevInfos(infos);
    return list;
}

json lan_dev_detail(const json& params)
{
    fnet_dev_detail_t* detail = nullptr;
    check(g_lib.getLanDevDetail(param_str(params, "ip").c_str(), require(params, "port").get<unsigned short>(),
                                param_str(params, "serialNumber").c_str(), param_str(params, "checkCode").c_str(), &detail,
                                TIMEOUT_MS),
          "get printer status");
    json result = detail_json(detail);
    g_lib.freeDevDetail(detail);
    return result;
}

json lan_light(const json& params)
{
    fnet_light_ctrl_t ctrl{};
    ctrl.lightStatus = require(params, "on").get<bool>() ? "open" : "close";
    check(g_lib.ctrlLanDevLight(param_str(params, "ip").c_str(), require(params, "port").get<unsigned short>(),
                                param_str(params, "serialNumber").c_str(), param_str(params, "checkCode").c_str(), &ctrl,
                                TIMEOUT_MS),
          "switch light");
    return json::object();
}

json dispatch(const std::string& method, const json& params)
{
    if (method == "hello")
        return hello(params);
    if (!g_lib.initialized)
        throw BridgeError{FNET_ERROR, "call hello first"};
    if (method == "loginPassword") return login_password(params);
    if (method == "sendSms") return send_sms(params);
    if (method == "loginSms") return login_sms(params);
    if (method == "refreshToken") return refresh_token(params);
    if (method == "startSession") return start_session(params);
    if (method == "signOut") return sign_out(params);
    if (method == "wanDevList") return wan_dev_list(params);
    if (method == "wanDevDetail") return wan_dev_detail(params);
    if (method == "lanDevList") return lan_dev_list(params);
    if (method == "lanDevDetail") return lan_dev_detail(params);
    if (method == "lanLight") return lan_light(params);
    throw BridgeError{FNET_ERROR, "unknown method: " + method};
}

} // namespace

int main()
{
    std::ios::sync_with_stdio(false);
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty())
            continue;
        json reply;
        try {
            const json request = json::parse(line);
            reply["id"]        = request.value("id", 0);
            reply["result"]    = dispatch(request.value("method", std::string()), request.value("params", json::object()));
            reply["ok"]        = true;
        } catch (const BridgeError& e) {
            reply["ok"]    = false;
            reply["code"]  = e.code;
            reply["error"] = e.message;
        } catch (const std::exception& e) {
            reply["ok"]    = false;
            reply["code"]  = FNET_ERROR;
            reply["error"] = e.what();
        }
        std::cout << reply.dump() << std::endl;
    }
    if (g_lib.initialized)
        g_lib.uninitlize();
    return 0;
}
