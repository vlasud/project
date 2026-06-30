#include "Services/HouseService/HouseService.h"

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>

namespace
{
constexpr float PI = 3.14159265358979323846f;
constexpr float EXIT_DISTANCE = 1.5f; // метров за спину создателя до точки выхода

float finiteOrZero(float value)
{
    return std::isfinite(value) ? value : 0.0f;
}
} // namespace

// ------------------------------------------------------------------- каталог

const std::vector<HouseService::CatalogEntry> &HouseService::catalog()
{
    // Канонические SA-MP координаты входа интерьеров. insideSpawn — точка спавна
    // внутри; пикап выхода HouseSystem ставит со смещением от неё (гард от
    // мгновенного ре-триггера выхода). Несколько пунктов делят один SA interior id
    // (5 — особняк/средний дом, 1 — квартира/склад): это один игровой интерьер с
    // разными точками спавна, что допустимо.
    static const std::vector<CatalogEntry> entries = {
        {"Мотель (комната)", 10, {2233.71f, -1115.21f, 1050.99f}, 0.0f},
        {"Малая квартира", 1, {2496.05f, -1695.28f, 1014.74f}, 180.0f},
        {"Средний дом", 5, {140.0f, 1366.96f, 1083.86f}, 0.0f},
        {"Особняк", 5, {1267.66f, -781.33f, 1091.91f}, 0.0f},
        {"Склад", 1, {1412.65f, -3.51f, 1000.92f}, 90.0f},
        {"Офис", 3, {383.80f, 173.84f, 1008.38f}, 0.0f},
    };
    return entries;
}

bool HouseService::catalogValid(int interiorIndex)
{
    return interiorIndex >= 0 && static_cast<std::size_t>(interiorIndex) < catalog().size();
}

// ------------------------------------------------------------------- запросы

const HouseService::House *HouseService::getHouse(int id) const
{
    const auto it = m_houses.find(id);
    return it == m_houses.end() ? nullptr : &it->second;
}

bool HouseService::ownsHouse(const std::string &ownerKey) const
{
    if (ownerKey.empty())
    {
        return false; // пустой ключ — ничейность, не владение
    }
    for (const auto &[id, house] : m_houses)
    {
        if (house.owner == ownerKey)
        {
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------------------- операции

Vector3 HouseService::backOf(const Vector3 &position, float angleDegrees, float distance)
{
    // SA-MP конвенция: «вперёд» = (-sin(a), cos(a)) при a в радианах, значит
    // «назад» = (sin(a), -cos(a)). Направление выхода пользователь проверяет в игре.
    const float a = angleDegrees * PI / 180.0f;
    return {position.x + std::sin(a) * distance, position.y - std::cos(a) * distance, position.z};
}

const HouseService::House *HouseService::createHouse(const Vector3 &creatorPos, float creatorAngle, int interiorIndex)
{
    if (!catalogValid(interiorIndex))
    {
        return nullptr;
    }
    // Лимит id: за ним VW_BASE + id переполнил бы int (UB). Отказываем в создании.
    if (m_nextId > MAX_HOUSE_ID)
    {
        return nullptr;
    }

    House house;
    house.id = m_nextId++;
    house.interiorIndex = interiorIndex;
    house.entrance = {finiteOrZero(creatorPos.x), finiteOrZero(creatorPos.y), finiteOrZero(creatorPos.z)};
    house.exitAngle = finiteOrZero(creatorAngle);
    house.exit = backOf(house.entrance, house.exitAngle, EXIT_DISTANCE);
    house.virtualWorld = VW_BASE + house.id;
    house.owner.clear(); // ничейный

    const auto [it, inserted] = m_houses.emplace(house.id, std::move(house));
    return &it->second;
}

bool HouseService::removeHouse(int id)
{
    return m_houses.erase(id) > 0;
}

bool HouseService::setOwner(int houseId, const std::string &ownerKey)
{
    const auto it = m_houses.find(houseId);
    if (it == m_houses.end())
    {
        return false; // дома нет
    }
    // Только память (зеркало БД); запись владения в house_owner делает HouseSystem.
    it->second.owner = ownerKey; // "" снимает владельца (ничейный)
    return true;
}

// ------------------------------------------------------------------- загрузка

void HouseService::loadHouse(const House &house)
{
    // Дубликат id из битого/правленого файла — отбрасываем (emplace не перезаписывает).
    m_houses.emplace(house.id, house);
}

void HouseService::finalizeLoad()
{
    int maxId = 0;
    for (const auto &[id, house] : m_houses)
    {
        maxId = std::max(maxId, id);
    }
    // m_nextId не выше MAX_HOUSE_ID+1: createHouse сам откажет на потолке, а vw
    // (VW_BASE + id) гарантированно не переполнит int даже на правленом файле.
    m_nextId = std::min(maxId + 1, MAX_HOUSE_ID + 1);
}

// ------------------------------------------------------------------- сериализация

std::string HouseService::serialize() const
{
    // Массив ОПИСАНИЙ домов через nlohmann/json (экранирование/кодирование — на
    // библиотеке). Только статический контент: владение (owner) живёт в БД
    // (house_owner), в файл НЕ пишется. Рантайм-хэндлы (пикапы/иконка) тоже не
    // сериализуются — их пересоздаёт HouseSystem из этих полей.
    nlohmann::json array = nlohmann::json::array();
    for (const auto &[id, house] : m_houses)
    {
        nlohmann::json item;
        item["id"] = house.id;
        item["interiorIndex"] = house.interiorIndex;
        item["entrance"] = {house.entrance.x, house.entrance.y, house.entrance.z};
        item["exit"] = {house.exit.x, house.exit.y, house.exit.z};
        item["exitAngle"] = house.exitAngle;
        item["virtualWorld"] = house.virtualWorld;
        array.push_back(std::move(item));
    }
    return array.dump(2);
}
