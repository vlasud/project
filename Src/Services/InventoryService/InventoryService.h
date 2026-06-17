#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include <array>
#include <string>
#include <utility>
#include <vector>

// Базовая система ВЕЩЕЙ — источник правды о предметах игроков онлайн. Сами типы
// предметов — часть кода (новый предмет = новая система-владелец), поэтому
// регистрируются кодом через registerItem() из конструкторов систем (как фракции
// саморегистрируются registerFaction). В БД — только данные: количество по типу
// на аккаунт (Sql/items.sql).
//
// Чистый контейнер состояния + реестр типов (как FactionService): БЕЗ
// бизнес-эффектов и БЕЗ обращений к БД внутри. Загрузку/сохранение и привязку к
// сессии делает InventorySystem; эффекты предмета (аптечка лечит) — система
// конкретного предмета (MedkitSystem). Сервис ни от кого не зависит (регистр
// конструирует его без параметров).
//
// Модель: предмет = целочисленный тип (itemType > 0) + количество. У каждого
// зарегистрированного типа — максимальный размер стека (maxStack >= 1): add не
// поднимает количество выше него. Реестр нужен и для валидации загрузки из БД —
// строка неизвестного/незарегистрированного типа отбрасывается, известный тип
// клампится к актуальному maxStack (устаревший избыток из БД не осядет).
//
// Имя предмета — utf-8 (отображение через utf8Tocp1251). Горячего пути нет — всё
// событийно (команда, диалог, старт/конец сессии). bounds-check playerId везде.
class InventoryService final : public IService
{
  public:
    // Определение зарегистрированного типа предмета (для дев-диалога/UI).
    struct ItemDef
    {
        int itemType = 0;
        std::string name; // utf-8
        int maxStack = 1;
    };

    // --- реестр типов (из конструкторов систем-владельцев предметов) ---
    // itemType > 0 и уникален; maxStack >= 1. Повтор/нарушение инвариантов
    // игнорируется с warning — реестр кодовый, дубликат это баг регистрации.
    void registerItem(int itemType, std::string name, int maxStack);
    const std::vector<ItemDef> &registeredItems() const;
    int maxStack(int itemType) const; // 0 — тип не зарегистрирован

    // --- операции с предметами игрока ---
    // Добавить n штук, НЕ выше maxStack типа. Возвращает СКОЛЬКО реально добавлено
    // (0..n). n <= 0 или незарегистрированный тип → 0. Переполнения нет (клампится
    // к maxStack).
    int add(int playerId, int itemType, int n);
    // Снять n штук АТОМАРНО: если текущее количество >= n — снимает и true, иначе
    // НИЧЕГО не снимает и false. n <= 0 → false. В минус не уводит.
    bool remove(int playerId, int itemType, int n);
    // Текущее количество типа у игрока. Невалидный/незарегистрированный → 0.
    int count(int playerId, int itemType) const;

    // --- вызываются InventorySystem (персист по сессии) ---
    // Загрузка из БД: применяет количества с КЛАМПОМ к актуальному maxStack;
    // незарегистрированные типы и мусор (count <= 0) отбрасываются. Перед загрузкой
    // слот должен быть очищен (reset на коннекте).
    void loadItems(int playerId, std::vector<std::pair<int, int>> typeQty);
    // Снимок ненулевых количеств для сохранения (пары тип-количество).
    std::vector<std::pair<int, int>> snapshot(int playerId) const;
    // Чистка слота (коннект/дисконнект), чтобы вещи одного аккаунта не утекли
    // следующему игроку в том же playerId-слоте.
    void reset(int playerId);

  private:
    const ItemDef *findDef(int itemType) const;

    struct Stack
    {
        int itemType = 0;
        int count = 0;
    };

    // Per-player хранилище: маленький вектор стеков (предметов мало). Запись по
    // типу — линейный поиск, но это событийный путь, не per-tick.
    using PlayerItems = std::vector<Stack>;

    std::vector<ItemDef> m_registry;            // кодовый реестр типов (мало записей)
    std::array<PlayerItems, MAX_PLAYERS> m_items;
};
