-- Владение домами. Применить вручную к схеме геймода.
-- Дома (id, интерьер, вход/выход, угол, vw) — статический контент, лежит в
-- houses.json (ставит дев). А вот «кто владеет домом» — динамические данные
-- АККАУНТА: в БД write-through (как членство фракций, faction_member), грузится
-- на старте и применяется к уже заспавненным домам.

-- house_id — дом из houses.json (PK: один владелец на дом). account_id — аккаунт
-- владельца (UNIQUE: один дом на аккаунт, БД-бэкстоп к in-memory проверке
-- ownsHouse). claimed_at — unix-время занятия (диагностика). Запись владения —
-- DELETE+INSERT в одной throwQuery (атомарно по house_id и account_id), удаление
-- дома стирает строку владения (нет осиротевшего владения).
CREATE TABLE IF NOT EXISTS `house_owner` (
    `house_id`   INT    NOT NULL,
    `account_id` BIGINT NOT NULL,
    `claimed_at` BIGINT NOT NULL DEFAULT 0,
    PRIMARY KEY (`house_id`),
    UNIQUE KEY `uq_account` (`account_id`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;
