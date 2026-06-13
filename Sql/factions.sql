-- Схема базовой системы фракций. Применить вручную к схеме геймода.
-- Сами фракции — часть кода (registerFaction с фиксированным id), в БД
-- только данные: ранги (правит лидер), члены (зарплата персональная,
-- прописывается лидером при найме) и бюджеты (из них платятся зарплаты).
-- Текст хранится в utf-8 (геймод конвертирует cp1251 <-> utf8 сам).

-- id рангов генерирует сервер (единственный писатель), не AUTO_INCREMENT.
-- У ранга только название и маска доступа: биты 0..7 — базовые
-- (1 приглашение, 2 увольнение, 4 бюджет), с бита 8 — биты организации.
-- is_default — стартовый «Без ранга»: есть у каждой фракции (геймод создаёт
-- сам при загрузке, если нет), удалить нельзя.
CREATE TABLE IF NOT EXISTS `faction_rank` (
    `id`          BIGINT      NOT NULL,
    `faction_id`  INT         NOT NULL,
    `name`        VARCHAR(64) NOT NULL,
    `permissions` BIGINT      NOT NULL DEFAULT 0,
    `is_default`  TINYINT     NOT NULL DEFAULT 0,
    PRIMARY KEY (`id`),
    KEY `idx_faction` (`faction_id`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;

-- Подопечные организации ранга фракции-куратора: лидер куратора передаёт
-- рангу в управление конкретные организации (назначение их лидеров в /gov).
CREATE TABLE IF NOT EXISTS `faction_rank_scope` (
    `rank_id`    BIGINT NOT NULL,
    `faction_id` INT    NOT NULL,
    PRIMARY KEY (`rank_id`, `faction_id`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;

-- skin — выбранный членом скин из пула организации (0 = не задан, тогда
-- применяется первый из пула). Член ВСЕГДА в скине организации, пока состоит;
-- /skin меняет выбор и пишет его сюда, на следующем заходе берётся отсюда.
CREATE TABLE IF NOT EXISTS `faction_member` (
    `account_id` BIGINT  NOT NULL,
    `faction_id` INT     NOT NULL,
    `rank_id`    BIGINT  NOT NULL,
    `is_leader`  TINYINT NOT NULL DEFAULT 0,
    `salary`     BIGINT  NOT NULL DEFAULT 0,
    `skin`       INT     NOT NULL DEFAULT 0,
    PRIMARY KEY (`account_id`),
    KEY `idx_faction_rank` (`faction_id`, `rank_id`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;

CREATE TABLE IF NOT EXISTS `faction_budget` (
    `faction_id` INT    NOT NULL,
    `budget`     BIGINT NOT NULL DEFAULT 0,
    PRIMARY KEY (`faction_id`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;

-- Банковский счёт аккаунта: зарплатные чеки и будущие операции банка.
CREATE TABLE IF NOT EXISTS `bank_account` (
    `account_id` BIGINT NOT NULL,
    `balance`    BIGINT NOT NULL DEFAULT 0,
    PRIMARY KEY (`account_id`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;

-- Выборы президента: партии, состояние выборов (одна строка id=1, срок —
-- unix-время, переживает рестарт) и голоса текущих выборов (один на аккаунт).
CREATE TABLE IF NOT EXISTS `party` (
    `id`                BIGINT       NOT NULL,
    `name`              VARCHAR(64)  NOT NULL,
    `description`       VARCHAR(256) NOT NULL DEFAULT '',
    `leader_account_id` BIGINT       NOT NULL,
    `leader_name`       VARCHAR(32)  NOT NULL DEFAULT '',
    PRIMARY KEY (`id`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;

CREATE TABLE IF NOT EXISTS `election` (
    `id`      INT     NOT NULL,
    `active`  TINYINT NOT NULL DEFAULT 0,
    `ends_at` BIGINT  NOT NULL DEFAULT 0,
    PRIMARY KEY (`id`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;

CREATE TABLE IF NOT EXISTS `election_vote` (
    `account_id` BIGINT NOT NULL,
    `party_id`   BIGINT NOT NULL,
    PRIMARY KEY (`account_id`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;

-- Миграция с прошлой версии схемы (если уже применял):
-- ALTER TABLE `faction_rank`
--     DROP COLUMN `access`,
--     ADD COLUMN `permissions` BIGINT NOT NULL DEFAULT 0,
--     ADD COLUMN `is_default`  TINYINT NOT NULL DEFAULT 0;
-- (либо, если успел применить версию с can_*: DROP их и ADD `permissions`)

-- Скин организации у члена (идемпотентно для живой БД; на MySQL 8.0.29+
-- поддерживается ADD COLUMN IF NOT EXISTS):
-- ALTER TABLE `faction_member` ADD COLUMN IF NOT EXISTS `skin` INT NOT NULL DEFAULT 0;
