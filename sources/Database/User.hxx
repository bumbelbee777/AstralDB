#pragma once

#include <stdint.h>
#include <string>
#include <vector>
#include <array>
#include <DS/EncryptedString.hxx>

namespace AstralDB {
enum class Permissions {
    Select = 1 << 0,
    Insert = 1 << 1,
    Update = 1 << 2,
    Delete = 1 << 3,
    Truncate = 1 << 4,
    References = 1 << 5,
    Trigger = 1 << 6,
    // Add more as needed
    All = Select | Insert | Update | Delete | Truncate | References | Trigger
};

struct RowColPermission {
    std::string Table;
    std::string RowId; // or primary key value
    std::string Column;
    Permissions Perms;
};

struct User {
    std::string Name;
    EncryptedString Password;
    std::vector<RowColPermission> FineGrainedPermissions;

    User(const std::string &Password);
    User(const std::string& Name, const std::string& Password, enum Permissions Permissions);
    User(User&&) noexcept = default;
    User& operator=(User&&) noexcept = default;
    User(const User&) = delete;
    User& operator=(const User&) = delete;

    bool VerifyPassword(const std::string& Input) const;

    static void SetDeviceSalt(const std::vector<uint8_t>& Salt);
    static void SetInstanceSalt(const std::vector<uint8_t>& Salt);
    static void SetSessionSalt(const std::vector<uint8_t>& Salt);
    static void RegenerateSessionSalt();
private:
    static std::vector<uint8_t> DeviceSalt_;
    static std::vector<uint8_t> InstanceSalt_;
    static std::vector<uint8_t> SessionSalt_;
    static std::array<uint8_t, 32> XChaChaKey_;
    static std::array<uint8_t, 24> XChaChaNonce_;
    static void InitSalts();
    std::vector<uint8_t> SaltAndHashPassword(const std::string& Password) const;
    static std::vector<uint8_t> GetCombinedSalt();
};
}