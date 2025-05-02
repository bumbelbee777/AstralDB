#pragma once

#include <stdint.h>
#include <string>
#include <DS/EncryptedString.hxx>

namespace AstralDB {
enum class Permissions {
    Read,
    Write,
    Admin = Read | Write
};

struct User {
    std::string Name;
    EncryptedString Password;
    Permissions Permissions;

    User() = delete;
};
}