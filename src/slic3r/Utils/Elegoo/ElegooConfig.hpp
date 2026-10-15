#pragma once

// ArtIN3D: endereços públicos da Elegoo usados pela conexão com impressoras, iguais aos
// que o ElegooSlicer gera em libslic3r_version.h. Os de telemetria ficam vazios: o
// ArtIN3D Slicer não envia dados de uso à Elegoo.

#define ELEGOO_INTERNAL_TESTING 0

#define ELEGOO_CHINA_IOT_URL "https://matrix.elegoo.com.cn"
#define ELEGOO_GLOBAL_IOT_URL "https://matrix.elegoo.com"
#define ELEGOO_CHINA_UPDATE_URL "https://device.elegoo.com.cn/device-api/v1/device-server/software/fdmSlicer/released"
#define ELEGOO_GLOBAL_UPDATE_URL "https://eu-device.elegoo.com/device-api/v1/device-server/software/fdmSlicer/released"
#define ELEGOO_CHINA_LOGIN_URL "https://www.nexprint.cn/account/slicer-login"
#define ELEGOO_GLOBAL_LOGIN_URL "https://www.nexprint.com/account/slicer-login"
#define ELEGOO_CHINA_ONLINE_MODEL_URL "https://www.nexprint.cn/elegooSlicer"
#define ELEGOO_GLOBAL_ONLINE_MODEL_URL "https://www.nexprint.com/elegooSlicer"
#define ELEGOO_CHINA_PLUGIN_UPDATE_URL ""
#define ELEGOO_GLOBAL_PLUGIN_UPDATE_URL ""
#define ELEGOO_CHINA_PROFILE_UPDATE_URL "https://device.elegoo.com.cn/device-api/v1/device-server/software/slicerParam/released/history"
#define ELEGOO_GLOBAL_PROFILE_UPDATE_URL "https://eu-device.elegoo.com/device-api/v1/device-server/software/slicerParam/released/history"
#define ELEGOO_CHINA_TELEMETRY_URL ""
#define ELEGOO_GLOBAL_TELEMETRY_URL ""
