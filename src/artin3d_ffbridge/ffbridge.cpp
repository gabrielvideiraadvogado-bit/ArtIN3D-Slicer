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
//   event:    {"event": "detail", "data": {...}}   (real-time connection, any time)
//
// The real-time connection never announces this client as a new PC login (Flash Studio's
// "sync login"), so a Flash Studio running on the same account stays signed in.
//
// The function declarations come from FlashNetwork.h of FlashForge/Orca-Flashforge 1.7.15
// (AGPL-3.0), which matches library version 3.4.x. Passwords are passed through to the
// library and never stored or logged here.

#include "FlashNetwork.h"

#include <nlohmann/json.hpp>

#include <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#include <fstream>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

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
    FNET_FUNC(getMqttConfig)
    FNET_FUNC(freeMqttConfig)
    FNET_FUNC(createConnection)
    FNET_FUNC(freeConnection)
    FNET_FUNC(connectionStop)
    FNET_FUNC(connectionSend)
    FNET_FUNC(connectionSendMulti)
    FNET_FUNC(freeWriteMultiResult)
    FNET_FUNC(connectionSubscribe)
    FNET_FUNC(allocString)
    FNET_FUNC(freeSyncLoginInfo)
    FNET_FUNC(freeSyncBindInfo)
    FNET_FUNC(freeSyncOnlineInfo)
    FNET_FUNC(freeUploadLogInfo)
    FNET_FUNC(freeSliceState)
    FNET_FUNC(freeJobInfo)
    FNET_FUNC(wanDevSendGcodeClound)
    FNET_FUNC(freeCloundGcodeData)
    FNET_FUNC(wanDevAddCloundJob)
    FNET_FUNC(freeAddCloudJobResults)
#undef FNET_FUNC
    std::string version;
    bool        initialized = false;
};

Lib g_lib;

// Replies come from the request loop, events from the library's connection threads.
std::mutex g_out_mutex;

void emit(const json& message)
{
    std::lock_guard<std::mutex> lock(g_out_mutex);
    std::cout << message.dump() << std::endl;
}

// Real-time (MQTT) connection to the Flashforge cloud.
struct Realtime
{
    std::mutex               mutex;
    void*                    conn = nullptr;
    std::string              client_id;
    std::string              access_token;
    std::string              user_topic;
    std::vector<std::string> common_topics;
    std::vector<std::string> dev_topics;
};

Realtime g_rt;

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
        LOAD(getMqttConfig) LOAD(freeMqttConfig) LOAD(createConnection) LOAD(freeConnection) LOAD(connectionStop)
        LOAD(connectionSend) LOAD(connectionSendMulti) LOAD(freeWriteMultiResult) LOAD(connectionSubscribe)
        LOAD(allocString) LOAD(freeSyncLoginInfo) LOAD(freeSyncBindInfo) LOAD(freeSyncOnlineInfo)
        LOAD(freeUploadLogInfo) LOAD(freeSliceState) LOAD(freeJobInfo)
        LOAD(wanDevSendGcodeClound) LOAD(freeCloundGcodeData) LOAD(wanDevAddCloundJob) LOAD(freeAddCloudJobResults)
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
                        {"serialNumber", str(d.serialNumber)}, {"location", str(d.location)},
                        {"devTopic", str(d.devTopic)}});
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

// ---- real-time connection ------------------------------------------------------------

std::vector<const char*> c_strings(const std::vector<std::string>& values)
{
    std::vector<const char*> out;
    for (const std::string& v : values)
        out.push_back(v.c_str());
    return out;
}

// Caller holds g_rt.mutex.
bool load_mqtt_config_locked()
{
    fnet_mqtt_config_t* config = nullptr;
    if (g_lib.getMqttConfig(g_rt.client_id.c_str(), g_rt.access_token.c_str(), &config, TIMEOUT_MS) != FNET_OK)
        return false;
    g_rt.user_topic = str(config->userTopic);
    g_rt.common_topics.clear();
    for (int i = 0; i < config->commonTopicCnt; ++i)
        g_rt.common_topics.push_back(str(config->commonTopics[i]));
    g_lib.freeMqttConfig(config);
    return true;
}

// Caller holds g_rt.mutex. Asks the printers to publish their full detail now.
void request_detail_locked(const std::vector<std::string>& topics)
{
    if (g_rt.conn == nullptr)
        return;
    // One request per printer: library 3.4.3 rejects the multi-topic form with a null payload.
    for (const std::string& topic : topics) {
        fnet_conn_write_data_t write{FNET_CONN_WRITE_UPDATE_DETAIL, nullptr, topic.c_str(), 1};
        g_lib.connectionSend(g_rt.conn, &write);
    }
}

void on_connected()
{
    std::lock_guard<std::mutex> lock(g_rt.mutex);
    if (g_rt.conn == nullptr)
        return;
    std::vector<std::string> topics = g_rt.common_topics;
    topics.push_back(g_rt.user_topic);
    topics.insert(topics.end(), g_rt.dev_topics.begin(), g_rt.dev_topics.end());
    std::vector<const char*>   ptrs = c_strings(topics);
    fnet_conn_subscribe_data_t sub{ptrs.data(), static_cast<int>(ptrs.size())};
    g_lib.connectionSubscribe(g_rt.conn, &sub);
    request_detail_locked(g_rt.dev_topics);
}

void status_callback(fnet_conn_status_t status, void*)
{
    const bool connected = status == FNET_CONN_STATUS_CONNECTED;
    emit({{"event", "connection"}, {"connected", connected}});
    if (connected)
        std::thread(on_connected).detach(); // the library's thread must not block
}

int update_callback(const char** client_id, void*)
{
    std::lock_guard<std::mutex> lock(g_rt.mutex);
    if (!load_mqtt_config_locked()) {
        emit({{"event", "connection"}, {"connected", false}, {"needsToken", true}});
        return 1;
    }
    *client_id = g_lib.allocString(g_rt.client_id.c_str(), static_cast<int>(g_rt.client_id.size()));
    return *client_id == nullptr ? 1 : 0;
}

void read_callback(fnet_conn_read_data_t* read, void*)
{
    switch (read->type) {
    case FNET_CONN_READ_DEVICE_DETAIL: {
        auto* detail = static_cast<fnet_dev_detail_t*>(read->data);
        if (detail != nullptr) {
            emit({{"event", "detail"}, {"data", detail_json(detail)}});
            g_lib.freeDevDetail(detail);
        }
        break;
    }
    case FNET_CONN_READ_SYNC_ONLINE:
    case FNET_CONN_READ_SYNC_OFFLINE: {
        auto* info = static_cast<fnet_sync_online_info_t*>(read->data);
        if (info != nullptr) {
            emit({{"event", read->type == FNET_CONN_READ_SYNC_ONLINE ? "online" : "offline"}, {"devId", str(info->devId)}});
            g_lib.freeSyncOnlineInfo(info);
        }
        break;
    }
    case FNET_CONN_READ_SYS_NOTIFY:
    case FNET_CONN_READ_UPDATE_NOTIFY:
    case FNET_CONN_READ_DEVICE_KEEP_ALIVE: g_lib.freeString(static_cast<char*>(read->data)); break;
    case FNET_CONN_READ_SYNC_LOGIN: g_lib.freeSyncLoginInfo(static_cast<fnet_sync_login_info_t*>(read->data)); break;
    case FNET_CONN_READ_SYNC_BIND_DEVICE:
    case FNET_CONN_READ_SYNC_UNBIND_DEVICE:
        g_lib.freeSyncBindInfo(static_cast<fnet_sync_bind_info_t*>(read->data));
        emit({{"event", "devicesChanged"}});
        break;
    case FNET_CONN_READ_UPLOAD_LOG: g_lib.freeUploadLogInfo(static_cast<fnet_upload_log_info_t*>(read->data)); break;
    case FNET_CONN_READ_SLICE_STATE: g_lib.freeSliceState(static_cast<fnet_slice_state_t*>(read->data)); break;
    case FNET_CONN_READ_JOB_DOWNLOAD:
    case FNET_CONN_READ_JOB_UNZIP: g_lib.freeJobInfo(static_cast<fnet_job_info_t*>(read->data)); break;
    default: break;
    }
    if (read->topic != nullptr)
        g_lib.freeString(read->topic);
}

void stop_realtime()
{
    void* conn = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_rt.mutex);
        conn      = g_rt.conn;
        g_rt.conn = nullptr;
    }
    if (conn != nullptr) {
        g_lib.connectionStop(conn);
        g_lib.freeConnection(conn);
    }
}

json connect(const json& params)
{
    stop_realtime();
    std::lock_guard<std::mutex> lock(g_rt.mutex);
    g_rt.client_id    = param_str(params, "clientId");
    g_rt.access_token = param_str(params, "accessToken");
    g_rt.dev_topics   = params.value("devTopics", std::vector<std::string>());
    if (!load_mqtt_config_locked())
        throw BridgeError{FNET_ERROR, "cannot get the real-time connection settings"};
    fnet_conn_settings_t settings{};
    settings.clientId       = g_rt.client_id.c_str();
    settings.statusCallback = status_callback;
    settings.updateCallback = update_callback;
    settings.readCallback   = read_callback;
    check(g_lib.createConnection(&g_rt.conn, &settings), "real-time connection");
    return json::object();
}

json set_token(const json& params)
{
    std::lock_guard<std::mutex> lock(g_rt.mutex);
    g_rt.access_token = param_str(params, "accessToken");
    return json::object();
}

json update_detail(const json&)
{
    std::lock_guard<std::mutex> lock(g_rt.mutex);
    request_detail_locked(g_rt.dev_topics);
    return json::object();
}

// Commands to one printer over the real-time connection.
json command(const json& params)
{
    const std::string topic  = param_str(params, "devTopic");
    const std::string action = param_str(params, "action");
    std::lock_guard<std::mutex> lock(g_rt.mutex);
    if (g_rt.conn == nullptr)
        throw BridgeError{FNET_ERROR, "not connected"};
    fnet_conn_write_data_t write{};
    write.topic = topic.c_str();
    write.qos   = 1;
    fnet_light_ctrl_t         light{};
    fnet_camera_stream_ctrl_t camera{};
    fnet_matl_station_ctrl_t  station{};
    if (action == "light") {
        light.lightStatus = require(params, "on").get<bool>() ? "open" : "close";
        write.type        = FNET_CONN_WRITE_LIGHT_CTRL;
        write.data        = &light;
    } else if (action == "camera") {
        camera.action = require(params, "open").get<bool>() ? "open" : "close";
        write.type    = FNET_CONN_WRITE_CAMERA_STREAM_CTRL;
        write.data    = &camera;
    } else if (action == "loadFilament" || action == "unloadFilament") {
        station.slotId = require(params, "slot").get<int>();
        station.action = action == "loadFilament" ? 0 : 1;
        write.type     = FNET_CONN_WRITE_MATL_STATION_CTRL;
        write.data     = &station;
    } else {
        throw BridgeError{FNET_ERROR, "unknown command: " + action};
    }
    check(g_lib.connectionSend(g_rt.conn, &write), action);
    return json::object();
}

// ---- sending a file to a cloud printer ----------------------------------------------
// Same sequence as Flash Studio (WanDevSendGcodeThd): upload the file and its thumbnail to
// Flashforge's storage, register a cloud job, then tell the printer over the real-time
// connection to fetch and (optionally) start it.

// Flashforge's job API identifies files by MD5 (an integrity check, not security).
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
std::string file_md5(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw BridgeError{FNET_ERROR, "cannot read " + path};
    CC_MD5_CTX ctx;
    CC_MD5_Init(&ctx);
    std::vector<char> buf(1 << 16);
    while (in) {
        in.read(buf.data(), buf.size());
        if (in.gcount() > 0)
            CC_MD5_Update(&ctx, buf.data(), static_cast<CC_LONG>(in.gcount()));
    }
    unsigned char digest[CC_MD5_DIGEST_LENGTH];
    CC_MD5_Final(digest, &ctx);
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (unsigned char b : digest) {
        out += hex[b >> 4];
        out += hex[b & 15];
    }
    return out;
}
#pragma clang diagnostic pop

long long file_size(const std::string& path)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    return in ? static_cast<long long>(in.tellg()) : 0;
}

std::string extension(const std::string& path)
{
    const size_t dot = path.find_last_of('.');
    return dot == std::string::npos ? std::string() : path.substr(dot + 1);
}

int upload_progress(long long now, long long total, void*)
{
    if (total > 0)
        emit({{"event", "sendProgress"}, {"progress", static_cast<double>(now) / static_cast<double>(total)}});
    return 0;
}

json send_file(const json& params)
{
    const std::string client_id   = param_str(params, "clientId");
    const std::string token       = param_str(params, "accessToken");
    const std::string dev_id      = param_str(params, "devId");
    const std::string serial      = param_str(params, "serialNumber");
    const std::string topic       = param_str(params, "devTopic");
    const std::string gcode_path  = param_str(params, "filePath");
    const std::string thumb_path  = param_str(params, "thumbPath");
    const std::string dst_name    = param_str(params, "dstName");
    const bool        print_now   = params.value("printNow", false);
    const bool        leveling    = params.value("leveling", false);
    const bool        timelapse   = params.value("timeLapse", false);
    const bool        use_station = params.value("useMatlStation", false);

    // Strings must outlive the library calls below.
    std::vector<std::string>             names, tool_colors, slot_colors;
    std::vector<fnet_material_mapping_t> mappings;
    const json                           mapping_list = params.value("mappings", json::array());
    names.reserve(mapping_list.size());
    tool_colors.reserve(mapping_list.size());
    slot_colors.reserve(mapping_list.size());
    for (const json& m : mapping_list) {
        names.push_back(m.value("materialName", std::string()));
        tool_colors.push_back(m.value("toolColor", std::string()));
        slot_colors.push_back(m.value("slotColor", std::string()));
        fnet_material_mapping_t mm{};
        mm.toolId            = m.value("toolId", 0);
        mm.slotId            = m.value("slotId", 0);
        mm.materialName      = names.back().c_str();
        mm.toolMaterialColor = tool_colors.back().c_str();
        mm.slotMaterialColor = slot_colors.back().c_str();
        mappings.push_back(mm);
    }

    fnet_send_gcode_data_t send{};
    send.gcodeFilePath       = gcode_path.c_str();
    send.thumbFilePath       = thumb_path.c_str();
    send.gcodeDstName        = dst_name.c_str();
    send.printNow            = print_now;
    send.levelingBeforePrint = leveling;
    send.timeLapseVideo      = timelapse;
    send.useMatlStation      = use_station;
    send.gcodeToolCnt        = static_cast<int>(mappings.size());
    send.materialMappings    = mappings.empty() ? nullptr : mappings.data();
    send.callback            = upload_progress;

    fnet_clound_gcode_data_t* stored = nullptr;
    check(g_lib.wanDevSendGcodeClound(client_id.c_str(), token.c_str(), &send, &stored, 10 * 60 * 1000), "upload file");

    const std::string gcode_type = extension(gcode_path);
    const std::string gcode_md5  = file_md5(gcode_path);
    const std::string thumb_type = extension(thumb_path);
    const std::string thumb_md5  = file_md5(thumb_path);
    const std::string thumb_name = dst_name.substr(0, dst_name.find_last_of('.') + 1) + gcode_type;
    const char*       dev_ids[]  = {dev_id.c_str()};

    fnet_clound_job_data_t job{};
    job.devIds              = dev_ids;
    job.devCnt              = 1;
    job.gcodeName           = dst_name.c_str();
    job.gcodeType           = gcode_type.c_str();
    job.gcodeMd5            = gcode_md5.c_str();
    job.gcodeSize           = file_size(gcode_path);
    job.thumbName           = thumb_name.c_str();
    job.thumbType           = thumb_type.c_str();
    job.thumbMd5            = thumb_md5.c_str();
    job.thumbSize           = file_size(thumb_path);
    job.bucketName          = stored->bucketName;
    job.endpoint            = stored->endpoint;
    job.gcodeStorageKey     = stored->gcodeStorageKey;
    job.gcodeStorageUrl     = stored->gcodeStorageUrl;
    job.thumbStorageKey     = stored->thumbStorageKey;
    job.thumbStorageUrl     = stored->thumbStorageUrl;
    job.unionId             = "";
    job.plateNo             = params.value("plate", 1);
    job.printNow            = print_now;
    job.levelingBeforePrint = leveling;
    job.timeLapseVideo      = timelapse;
    job.useMatlStation      = use_station;
    job.gcodeToolCnt        = send.gcodeToolCnt;
    job.materialMappings    = send.materialMappings;

    fnet_add_clound_job_result_t* results = nullptr;
    int                           count   = 0;
    const int add_ret = g_lib.wanDevAddCloundJob(client_id.c_str(), token.c_str(), &job, &results, &count, TIMEOUT_MS);
    if (add_ret != FNET_OK) {
        g_lib.freeCloundGcodeData(stored);
        check(add_ret, "create print job");
    }
    std::string job_id;
    int         job_error = FNET_ADD_CLOUND_JOB_OK;
    for (int i = 0; i < count; ++i)
        if (str(results[i].devId) == dev_id) {
            job_id    = str(results[i].jobId);
            job_error = results[i].error;
        }
    g_lib.freeAddCloudJobResults(results, count);
    if (job_id.empty() || job_error != FNET_ADD_CLOUND_JOB_OK) {
        g_lib.freeCloundGcodeData(stored);
        static const char* reasons[] = {"ok", "printer busy", "printer not found", "Flashforge server error"};
        throw BridgeError{FNET_ERROR, std::string("print job refused: ") +
                                          (job_error >= 0 && job_error <= 3 ? reasons[job_error] : "unknown error")};
    }

    const char* serials[] = {serial.c_str()};
    const char* job_ids[] = {job_id.c_str()};
    job.devSerialNumbers  = serials;
    job.jobIds            = job_ids;
    int start_ret         = FNET_ERROR;
    {
        std::lock_guard<std::mutex> lock(g_rt.mutex);
        if (g_rt.conn != nullptr) {
            fnet_conn_write_data_t write{FNET_CONN_WRITE_START_CLOUND_JOB, &job, topic.c_str(), 1};
            start_ret = g_lib.connectionSend(g_rt.conn, &write);
        }
    }
    g_lib.freeCloundGcodeData(stored);
    check(start_ret, "notify the printer");
    return {{"jobId", job_id}};
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
    if (method == "connect") return connect(params);
    if (method == "setToken") return set_token(params);
    if (method == "updateDetail") return update_detail(params);
    if (method == "command") return command(params);
    if (method == "sendFile") return send_file(params);
    if (method == "disconnect") { stop_realtime(); return json::object(); }
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
        emit(reply);
    }
    if (g_lib.initialized) {
        stop_realtime();
        g_lib.uninitlize();
    }
    return 0;
}
