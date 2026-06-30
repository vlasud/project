-- Выбор точки спавна игрока (/setspawn). Применить вручную к схеме геймода.
-- «Где появляться при входе в игру и после смерти» — динамические данные
-- АККАУНТА: в БД write-through (как членство фракций faction_member и владение
-- домами house_owner), грузятся на старте сессии и применяются к спавну.

-- account_id — аккаунт игрока (PK: один выбор на аккаунт). choice — индекс
-- выбора (0 — ЖД вокзал/дефолт, 1 — дом, 2 — место работы/база фракции).
-- Неизвестное значение при загрузке нормализуется в 0 (вокзал). Нет строки —
-- дефолтный выбор (вокзал). Запись — REPLACE (DELETE+INSERT) в одной throwQuery
-- (атомарно по account_id).
CREATE TABLE IF NOT EXISTS `player_spawn` (
    `account_id` BIGINT NOT NULL,
    `choice`     INT    NOT NULL DEFAULT 0,
    PRIMARY KEY (`account_id`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;
