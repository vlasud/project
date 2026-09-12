#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "types.hpp"
#include <array>
#include <optional>

class ReturnPointSystem;

// Хранилище точки возврата аккаунта — где игрок был в конце прошлой сессии
// (см. Docs/ReturnPoint.md). Бизнес-фича персиста, НЕ Core: загрузку/сохранение
// (`player_return_point`) и предложение вернуться делает ReturnPointSystem.
//
// Слот держит четыре независимых факта:
//  * loaded     — async-select по старту сессии завершился (точки при этом может и
//                 не быть: новый аккаунт или битая строка);
//  * point      — сама точка, ВСЕГДА прошедшая валидацию (см. load): нефинитные и
//                 запредельные координаты наружу не выходят;
//  * spawned    — логин-спавн сессии пройден. До него принятая позиция — не
//                 реальное место игрока (спектейт авторизации), и снимок затёр бы в
//                 БД настоящую точку аккаунта, оборвись сессия раньше спавна (тот
//                 же по смыслу гейт, что areWeaponsApplied у оружия);
//  * promptDone — предложение вернуться уже сделано (один раз за сессию).
class ReturnPointService final : public IService
{
    friend ReturnPointSystem;

  public:
    // Границы годной точки (те же, что кламп точки спавна в PlayerSpawnService):
    // интерьеры лежат далеко за картой, поэтому предел щадящий. Публичные — по ним
    // фильтрует и вычитка из БД, до сужения double -> float.
    static constexpr float WORLD_LIMIT = 20000.0f;
    static constexpr int MAX_INTERIOR = 255;

    struct Point
    {
        Vector3 position{};
        float angle = 0.0f; // yaw, градусы
        unsigned interior = 0;
        int virtualWorld = 0;
    };

    // Точка годна: координаты финитны и в пределах мира, интерьер в диапазоне,
    // виртуальный мир не отрицателен. ОДНО правило и для снимка, и для чтения строки
    // из БД — пишем не шире, чем читаем, иначе мусор молча превратился бы в другое
    // место. Угол сюда не входит: он косметика и нормализуется обеими сторонами
    // (нефинитный -> 0). «Есть ли вообще принятая позиция» этим правилом НЕ
    // проверяется (нулевой вектор для мира легален) — это гейт вызывателя снимка
    // (PlayerLocationService::hasPosition).
    static bool isValid(const Point &point);

    bool isLoaded(int playerId) const;
    // Точка прошлой сессии или nullptr: не загружено / нет строки / строка битая.
    const Point *getPoint(int playerId) const;

    bool isSpawned(int playerId) const;
    // Отметить пройденный логин-спавн и ЗАПОМНИТЬ его момент: срок предложения
    // вернуться считается от спавна, а не от показа диалога (показ может опоздать —
    // загрузка точки уходит в очередь при занятом пуле БД).
    void markSpawned(int playerId, TimePoint now);
    // Момент логин-спавна; вне диапазона id / до спавна — TimePoint{}.
    TimePoint spawnedAt(int playerId) const;

    bool isPromptDone(int playerId) const;
    void markPromptDone(int playerId);

  private:
    // --- вызывается ReturnPointSystem ---
    // Результат загрузки: nullopt или невалидная точка -> «точки нет». loaded
    // взводится в любом случае — гейт сохранения смотрит на spawned, не на точку.
    void load(int playerId, const std::optional<Point> &point);
    void reset(int playerId);

    struct Slot
    {
        bool loaded = false;
        bool hasPoint = false;
        bool spawned = false;
        bool promptDone = false;
        TimePoint spawnedAt{}; // момент логин-спавна (якорь срока предложения)
        Point point{};
    };

    std::array<Slot, MAX_PLAYERS> m_slots;
};
