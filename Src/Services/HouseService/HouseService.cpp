#include "Services/HouseService/HouseService.h"

#include "Utils/Geometry/Geometry.h"
#include "Utils/Sanitize.h"
#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
#include <utility>

namespace
{
constexpr float EXIT_DISTANCE = 1.5f; // метров за спину создателя до точки выхода

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

const HouseService::House *HouseService::houseOf(const std::string &ownerKey) const
{
    if (ownerKey.empty())
    {
        return nullptr; // пустой ключ — ничейность, дома нет
    }
    for (const auto &[id, house] : m_houses)
    {
        if (house.owner == ownerKey)
        {
            return &house; // один дом на игрока — первый совпавший и есть его дом
        }
    }
    return nullptr;
}

// ------------------------------------------------------------------- операции

Vector3 HouseService::backOf(const Vector3 &position, float angleDegrees, float distance)
{
    // «Назад» по углу создателя (SA-MP конвенция) — общий хелпер Geometry.
    return Geometry::backOf(position, angleDegrees, distance);
}

const HouseService::House *HouseService::createHouse(const Vector3 &creatorPos, float creatorAngle, int interiorIndex,
                                                     int parkingCap, std::int64_t price)
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
    house.entrance = {Utils::finiteOrZero(creatorPos.x), Utils::finiteOrZero(creatorPos.y), Utils::finiteOrZero(creatorPos.z)};
    house.exitAngle = Utils::finiteOrZero(creatorAngle);
    house.exit = backOf(house.entrance, house.exitAngle, EXIT_DISTANCE);
    house.virtualWorld = VW_BASE + house.id;
    house.owner.clear(); // ничейный
    // Кламп капа в [MIN, MAX]: страховка даже если вызывающий уже проверил диапазон.
    house.parkingCap = std::clamp(parkingCap, MIN_PARKING_CAP, MAX_PARKING_CAP);
    house.price = std::max<std::int64_t>(price, 0);

    const auto [it, inserted] = m_houses.emplace(house.id, std::move(house));
    return &it->second;
}

bool HouseService::setPrice(int id, std::int64_t price)
{
    const auto it = m_houses.find(id);
    if (it == m_houses.end())
    {
        return false;
    }
    it->second.price = std::max<std::int64_t>(price, 0);
    return true;
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
    // Только память (зеркало БД); запись владения в house_owner делает HouseSystem
    // по нотификации ниже (единая точка персиста для занятия/передачи/выселения).
    const std::string oldKey = it->second.owner;
    it->second.owner = ownerKey; // "" снимает владельца (ничейный)
    for (const OwnerChangedObserver &observer : m_ownerChangedObservers)
        observer(houseId, oldKey, ownerKey);
    return true;
}

void HouseService::subscribeOwnerChanged(OwnerChangedObserver observer)
{
    if (!observer)
        return;
    m_ownerChangedObservers.push_back(std::move(observer));
}

bool HouseService::setOwnerSilent(int houseId, const std::string &ownerKey)
{
    const auto it = m_houses.find(houseId);
    if (it == m_houses.end())
    {
        return false;
    }
    it->second.owner = ownerKey; // без нотификации — только откат памяти при сбое персиста
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

void HouseService::subscribeOwnershipLoaded(OwnershipLoadedObserver observer)
{
    if (!observer)
        return;
    // Поздняя подписка после загрузки — колбэк сразу (one-shot не теряется).
    if (m_ownershipLoaded)
    {
        observer();
        return;
    }
    m_ownershipLoadedObservers.push_back(std::move(observer));
}

void HouseService::markOwnershipLoaded()
{
    if (m_ownershipLoaded)
        return; // идемпотентно: оповещаем подписчиков ровно один раз
    m_ownershipLoaded = true;
    for (const OwnershipLoadedObserver &observer : m_ownershipLoadedObservers)
        observer();
    m_ownershipLoadedObservers.clear(); // подписки больше не нужны (one-shot)
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
        item["parkingCap"] = house.parkingCap;
        item["price"] = house.price;
        array.push_back(std::move(item));
    }
    return array.dump(2);
}
