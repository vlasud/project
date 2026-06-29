-- Схема бизнес-фичи «Семьи». Применить вручную к схеме геймода.
-- Семьи — player-created группы: создаются игроками в рантайме, в БД лежат
-- целиком (write-through, как членство фракций) и переживают рестарт.
-- Текст хранится в utf-8 (геймод конвертирует cp1251 <-> utf8 сам).

-- id семьи генерирует сервер (единственный писатель), не AUTO_INCREMENT:
-- m_nextId = max(id)+1 на загрузке, INSERT с явным id. name уникально по
-- серверу. owner_account_id — старейшина (владение наследует старейший по
-- joined_at при его уходе). created_at/joined_at — unix-время.
-- Коллация name — utf8mb4_bin (байт-точная): БД-UNIQUE совпадает с in-memory
-- проверкой уникальности (ASCII-регистронезависимой). Дефолтная ai_ci свернула
-- бы регистр по всему Юникоду — кириллические варианты ('Корлеоне'/'корлеоне')
-- in-memory считаются разными и прошли бы, а INSERT упал бы на UNIQUE
-- (рассинхрон). bin строже не делает: ASCII-fold идёт первым и отвергает раньше.
CREATE TABLE IF NOT EXISTS `family` (
    `id`               INT         NOT NULL,
    `name`             VARCHAR(64) NOT NULL COLLATE utf8mb4_bin,
    `owner_account_id` BIGINT      NOT NULL,
    `created_at`       BIGINT      NOT NULL DEFAULT 0,
    PRIMARY KEY (`id`),
    UNIQUE KEY `uniq_name` (`name`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;

-- Членство: одна семья на аккаунт (account_id — PK). name — ник на момент
-- вступления (для состава семьи), joined_at — стаж (наследование владения).
CREATE TABLE IF NOT EXISTS `family_member` (
    `account_id` BIGINT      NOT NULL,
    `family_id`  INT         NOT NULL,
    `name`       VARCHAR(32) NOT NULL DEFAULT '',
    `joined_at`  BIGINT      NOT NULL DEFAULT 0,
    PRIMARY KEY (`account_id`),
    KEY `idx_family` (`family_id`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;
