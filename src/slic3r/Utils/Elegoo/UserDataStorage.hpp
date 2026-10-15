#pragma once

#include <string>
#include <vector>

#if !defined(_WIN32)
#include <openssl/evp.h>
#include <openssl/rand.h>
#endif

namespace Slic3r {

class UserDataStorage {
public:
    explicit UserDataStorage(const std::string& filePath);
    
    bool save(const std::string& jsonString);
    std::string load();

private:
    std::string mFilePath;

#if defined(_WIN32)
    bool encryptWindows(const std::string& plain, std::vector<uint8_t>& encrypted);
    bool decryptWindows(const std::vector<uint8_t>& encrypted, std::string& plain);
#else
    bool encryptAES(const std::string& plain, std::vector<uint8_t>& encrypted);
    bool decryptAES(const std::vector<uint8_t>& encrypted, std::string& plain);
#endif
};

} // namespace Slic3r

