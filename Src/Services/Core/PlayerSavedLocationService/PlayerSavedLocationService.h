#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "types.hpp"
#include <array>

// Личная закладка координат админа (`/savepos` + `/tppos`). Пер-игрок: позиция,
// интерьер и виртуальный мир, запомненные исполнителем для быстрого возврата.
//
// Только память, без БД — закладка живёт ровно сессию и сбрасывается на конце
// (AdminSystem зовёт reset на subscribeEnd). Сброс обязателен: иначе координаты
// одного админа «протекли» бы в переиспользованный слот к следующему игроку.
//
// Чистый контейнер состояния: O(1) доступ, никакой валидации позиции (её
// записывает командой только сам админ из своей принятой позиции).
class PlayerSavedLocationService final : public IService
{
  public:
    struct Saved
    {
        Vector3 position{};
        unsigned interior = 0;
        int virtualWorld = 0;
        bool valid = false; // false — закладки нет (get вернёт nullptr)
    };

    // Запомнить точку (valid=true). Вне диапазона id — игнор.
    void save(int playerId, const Vector3 &position, unsigned interior, int virtualWorld);

    // Закладка игрока или nullptr (нет сохранённой / id вне диапазона).
    const Saved *get(int playerId) const;

    // Сброс слота (зовётся на конце сессии).
    void reset(int playerId);

  private:
    std::array<Saved, MAX_PLAYERS> m_saved{};
};
