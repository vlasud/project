#pragma once

#include <cctype>
#include <string_view>

namespace Utils
{
// Имя файла пресета: только [A-Za-z0-9_-], непусто, не длиннее 64 символов.
// Белый список сам исключает '/', '\\', '..', ':' — иначе путь мог бы выйти
// из каталога пресетов (path traversal) или создать ADS на NTFS.
inline bool isValidPresetName(std::string_view name)
{
    if (name.empty() || name.size() > 64)
    {
        return false;
    }
    for (char c : name)
    {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-')
        {
            return false;
        }
    }
    return true;
}
} // namespace Utils
