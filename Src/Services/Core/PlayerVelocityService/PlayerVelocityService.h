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
// Отдельно от скорости сервис следит за разворотами модели: серия мгновенных
// разворотов в скользящем окне — CLEO quick turn (анимация поворота столько не
// успевает). Считается только пешком: в транспорте угол ведёт машина.
//
// Валидация скорости — по устойчивому превышению (набрать 2 с над лимитом, не
// разовый пик; под лимитом набранное убывает вдвое медленнее, чем росло, поэтому
// пилящая скорость не отменяет детект, а всплеск рассасывается):
//  * пешком на земле: горизонталь и подъём ограничены (спидхак, airbreak вверх);
//    пока игрок прыгает (bunny hop — легальный разгон до 11-12 м/с), горизонтальный
//    лимит выше: отличаем разгон прыжками от ровного бега на той же скорости;
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
        bool quickTurn = false; // серия мгновенных разворотов модели
        std::string detail;
    };

    // --- дев-диагностика (AntiCheatTestSystem) ---
    // Какая ветка лимитов выбрана на последнем сэмпле и сколько миллисекунд скорость
    // держится над лимитом. Без этого «скорость высокая, а нарушения нет» неразрешимо:
    // ветка падения/сёрфа снимает пеший лимит, а окно устойчивости может сбрасываться.
    const char *lastBranch(int playerId) const;
    int overMs(int playerId) const;

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
        float overSeconds = 0.0f; // накоплено времени над лимитом (под лимитом убывает)
        TimePoint lastJumpAt;     // последний отрыв от земли — окно «прыжкового» лимита
        // Разворот модели: угол прошлого сэмпла и счётчик мгновенных разворотов
        // в скользящем окне (одиночный разворот легален, серия — нет).
        bool hasYaw = false;
        float lastYaw = 0.0f;
        TimePoint snapWindowStart;
        std::uint8_t snapCount = 0;
        // Ветка лимитов последнего сэмпла — только для дев-диагностики.
        enum class Branch : std::uint8_t
        {
            None,
            Foot,
            FootHop, // пешком, но с недавним отрывом от земли — лимит выше
            Falling,
            Vehicle,
        } branch = Branch::None;
    };

    PlayerLocationService *m_location = nullptr;

    std::array<State, MAX_PLAYERS> m_state;
};
