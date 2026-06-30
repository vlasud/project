#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "types.hpp"
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

class HouseSystem;

// Дома — player houses (бизнес-фича, НЕ Core). Дом — пикап ВХОДА в позиции
// создателя + ИНТЕРЬЕР из каталога готовых интерьеров; внутри — пикап ВЫХОДА.
// id дома — порядковый номер создания.
//
// ДВА РАЗНЫХ ИСТОЧНИКА ПРАВДЫ:
//  * ОПИСАНИЕ дома (статический контент, ставит дев) — houses.json рядом с
//    сервером (рабочая директория): id, интерьер, точки входа/выхода, угол, vw.
//    Сериализуется/грузится этим сервисом (serialize/loadHouse).
//  * ВЛАДЕНИЕ (house -> аккаунт) — динамические данные АККАУНТА, живут в БД
//    (таблица house_owner, write-through как членство фракций). В json НЕ пишутся.
//    House.owner — лишь in-memory зеркало владения; наполняется из БД на старте
//    (HouseSystem грузит house_owner ПОСЛЕ спавна домов) и на занятии. Запись в
//    БД делает HouseSystem; setOwner здесь меняет только память.
//
// Рантайм-хэндлы пикапов/иконки в JSON НЕ хранятся — их держит HouseSystem
// (m_runtime) и пересоздаёт при загрузке. Сериализуется только то, что
// восстанавливает дом: id, интерьер, точки входа/выхода, угол выхода, мир.
//
// id генерирует сервер (единственный писатель): m_nextId = max(id)+1 на загрузке.
// vw дома — уникальный (VW_BASE + id), чтобы интерьеры разных домов не
// пересекались; снаружи (вход/выход/иконка) — основной мир (vw 0).
class HouseService final : public IService
{
    friend HouseSystem;

  public:
    // База уникального виртуального мира дома: внутри изоляция, снаружи всегда 0.
    static constexpr int VW_BASE = 2000000;
    // Потолок id дома: VW_BASE + MAX_HOUSE_ID заведомо < INT_MAX, vw не переполнится.
    static constexpr int MAX_HOUSE_ID = 1000000;

    struct House
    {
        int id = 0;
        int interiorIndex = 0;   // индекс в каталоге интерьеров (catalog())
        Vector3 entrance{};      // пикап входа (позиция создателя) — основной мир
        Vector3 exit{};          // точка появления при выходе — за спиной создателя
        float exitAngle = 0.0f;  // угол поворота создателя (yaw, градусы) на момент создания
        int virtualWorld = 0;    // уникальный мир интерьера дома
        std::string owner;       // ключ аккаунта (std::to_string(accountId)); "" — ничейный.
                                 // Зеркало БД (house_owner), а НЕ json — наполняется на старте/занятии.
    };

    // Готовый интерьер: имя в дев-меню + interior id SA + фикс. точка спавна
    // внутри и угол. Координаты — канонические SA-MP координаты входа интерьеров.
    struct CatalogEntry
    {
        const char *name;        // отображается в LIST (utf-8)
        int interiorId;          // SA interior id
        Vector3 insideSpawn;     // точка спавна внутри интерьера
        float insideAngle;       // угол поворота при появлении внутри (yaw, градусы)
    };

    // Каталог готовых интерьеров (фиксированная таблица).
    static const std::vector<CatalogEntry> &catalog();
    static bool catalogValid(int interiorIndex); // bounds-проверка индекса каталога

    // --- запрос состояния ---
    const House *getHouse(int id) const; // nullptr — нет такого дома
    const std::unordered_map<int, House> &houses() const
    {
        return m_houses;
    }
    std::size_t count() const
    {
        return m_houses.size();
    }
    // Владеет ли ownerKey хотя бы одним домом (один дом на игрока). ownerKey —
    // ключ владельца (std::to_string(accountId)); пустой всегда false (ничейный
    // дом не «принадлежит» никому). Линейно по m_houses (число домов мало —
    // холодный путь занятия/гейта, не per-tick).
    bool ownsHouse(const std::string &ownerKey) const;
    // Дом, которым владеет ownerKey, или nullptr (нет дома). Один дом на игрока,
    // поэтому возвращается первый совпавший. ownerKey — ключ владельца
    // (std::to_string(accountId)); пустой всегда nullptr. Линейно по m_houses
    // (мало домов, холодный путь — точка спавна «Дом»), не per-tick.
    const House *houseOf(const std::string &ownerKey) const;

    // --- операции (источник правды; персист делает HouseSystem) ---
    // Создать дом в позиции создателя. creatorAngle — yaw создателя (градусы);
    // точка выхода считается за спиной по этому углу. interiorIndex обязан быть
    // валидным (вызывающий проверяет catalogValid). Возвращает указатель на
    // созданный дом (живёт в m_houses) или nullptr при невалидном индексе либо
    // достигнутом лимите id (MAX_HOUSE_ID).
    const House *createHouse(const Vector3 &creatorPos, float creatorAngle, int interiorIndex);
    // Удалить дом. false — дома нет (вызывающий снимает рантайм-хэндлы заранее).
    bool removeHouse(int id);
    // Выставить владельца дома в ПАМЯТИ (зеркало БД). ownerKey — ключ аккаунта
    // (std::to_string(accountId)); "" снимает владельца (ничейный). false — дома
    // нет. Запись владения в БД (house_owner) и пересоздание иконки делает
    // HouseSystem — этот метод НЕ трогает ни json, ни БД.
    bool setOwner(int houseId, const std::string &ownerKey);

    // --- сериализация JSON (только ОПИСАНИЕ домов, без владения) ---
    std::string serialize() const; // весь набор домов в JSON (для записи файла)

  private:
    // --- вызывается HouseSystem ---
    // Загрузить дом из распарсенной строки (на старте). Дубликат id отбрасывается.
    void loadHouse(const House &house);
    // Финализация загрузки: выставить m_nextId = max(id)+1, но не выше MAX_HOUSE_ID+1.
    void finalizeLoad();

    // Точка выхода за спиной создателя по его углу (SA-MP конвенция «вперёд» =
    // (-sin, cos), значит «назад» = (sin, -cos)).
    static Vector3 backOf(const Vector3 &position, float angleDegrees, float distance);

    std::unordered_map<int, House> m_houses;
    int m_nextId = 1; // сервер — единственный писатель id домов
};
