#pragma once

#include "../../Macro.h"
#include "../IService.h"
#include "types.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Журнал нарушений игроков. Системы-детекторы (например, PlayerAnimationSystem)
// фиксируют сюда подозрительные события через record(). Сам сервис ничего не
// предпринимает — решение, что делать с игроком (кик, бан, предупреждение),
// примет будущая система-античит, читая эти записи. Так детекторы и реакция
// разделены: добавить новый детектор или поменять политику реакции можно
// независимо друг от друга.
class AntiCheatService final : public IService
{
  public:
    enum class ViolationType : std::uint8_t
    {
        ForcedAnimationEscape, // игрок вышел из непрерываемой серверной анимации
    };

    struct Violation
    {
        ViolationType type;
        TimePoint time;
        std::string detail; // детали для лога (что требовалось / что заявил клиент)
    };

    struct PlayerRecord
    {
        std::uint32_t total = 0; // всего зафиксировано (не урезается лимитом recent)
        TimePoint firstAt;       // время первого нарушения
        TimePoint lastAt;        // время последнего нарушения
        std::vector<Violation> recent; // последние нарушения с деталями (ограничено)
    };

    // Зафиксировать нарушение. Вызывают системы-детекторы.
    void record(int playerId, ViolationType type, std::string detail, TimePoint now);

    // Для будущего античита: чтение и сброс.
    const PlayerRecord &get(int playerId) const;
    std::uint32_t count(int playerId) const;
    bool flagged(int playerId) const;
    void clear(int playerId);

  private:
    static constexpr std::size_t RECENT_LIMIT = 20;

    std::array<PlayerRecord, MAX_PLAYERS> m_records;
};
