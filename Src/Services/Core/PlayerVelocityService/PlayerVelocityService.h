#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include "player.hpp"
#include "types.hpp"
#include <array>
#include <cstdint>
#include <string>

// Сервис скорости — серверно-вычисляемая velocity игрока и её валидация.
//
// Скорость НЕ берётся из клиентского sync (player.getVelocity() — то, что клиент
// о себе заявляет), а выводится из последовательных ПРИНЯТЫХ позиций
// PlayerLocationService: (pos - prevPos) / dt, со сглаживанием EMA. Это даёт
// единственный достоверный источник velocity для игровой логики.
//
// Сэмплы через разрыв непрерывности позиции (телепорт, спавн, грейс интерьера,
// пауза, байпас) пропускаются по счётчику разрывов LocationService — легальный
// телепорт не выглядит как скорость в тысячи м/с.
//
// Валидация — по устойчивому превышению (окно 1.5 с, не разовый пик):
//  * пешком на земле: горизонталь и подъём ограничены (спидхак, airbreak вверх);
//  * пешком в падении (vz < порога вниз): лимиты щедрые — прыжок с самолёта
//    несёт его горизонтальную скорость, вертикаль в свободном падении велика;
//  * сёрф на транспорте/объекте: транспортные лимиты;
//  * в транспорте: общий щедрый лимит (самые быстрые самолёты).
class PlayerVelocityService final : public IService
{
  public:
    // Вызывается PlayerVelocitySystem при создании.
    void bind(PlayerLocationService &location);

    // --- источник правды velocity (м/с) ---
    Vector3 getVelocity(int playerId) const;
    float getSpeed(int playerId) const;           // модуль полной скорости
    float getHorizontalSpeed(int playerId) const; // модуль по XY
    float getVerticalSpeed(int playerId) const;   // по Z со знаком (минус — падение)

    struct VerifyOutcome
    {
        bool speedHack = false;
        std::string detail;
    };

    // --- вызывается PlayerVelocitySystem ---
    VerifyOutcome sample(IPlayer &player, TimePoint now); // каждый апдейт игрока
    void reset(int playerId);

  private:
    struct State
    {
        bool hasSample = false;
        std::uint32_t lastDiscontinuity = 0;
        Vector3 lastPosition{};
        TimePoint lastSample;
        Vector3 velocity{};  // сглаженная (EMA)
        TimePoint overSince; // с какого момента скорость стабильно над лимитом
    };

    PlayerLocationService *m_location = nullptr;

    std::array<State, MAX_PLAYERS> m_state;
};
