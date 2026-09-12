-- =====================================================================
-- Гейммод — полная схема БД (сводный файл всех миграций).
-- Автособрано из Sql/*.sql на 2026-07-01. Применять к схеме гейммода.
-- Файл ИДЕМПОТЕНТЕН и гоняется целиком повторно: таблицы — CREATE TABLE IF NOT
-- EXISTS, ALTER'ы — под проверкой information_schema (MySQL не знает IF NOT EXISTS
-- в ADD COLUMN/ADD INDEX, а голый ALTER на применённой базе оборвал бы прогон).
-- Порядок некритичен.
-- ИСКЛЮЧЁН family_vehicle.sql — устарел, заменён parked_vehicle.sql (НЕ применять).
-- =====================================================================


-- ============ [1/11] players.sql ============

-- Схема таблицы аккаунтов. Применить вручную к схеме геймода.
-- Аккаунт создаётся при регистрации (INSERT без id — id генерирует БД,
-- AUTO_INCREMENT), идентифицируется по нику. Текст хранится в utf-8 (геймод
-- конвертирует cp1251 <-> utf8 сам).
--
--   id            — первичный ключ аккаунта (он же account_id в остальных
--                   таблицах: faction_member, bank_account, election_vote, ...).
--   name          — ник игрока, формат «Имя_Фамилия» (латиница), до 24 символов
--                   (предел SA-MP). Уникален: вход и регистрация ищут по нему.
--   password_hash — строка-хеш пароля от libsodium crypto_pwhash_str
--                   (crypto_pwhash_STRBYTES = 128, ASCII).
--   sex           — пол: 0 — мужской, 1 — женский (ESex).
--   skin          — личный (гражданский) скин аккаунта: в нём игрок появляется
--                   на входе и в него возвращается при увольнении из фракции
--                   (член ОТОБРАЖАЕТСЯ в скине организации, но личный скин
--                   хранится здесь неизменным). Дефолт 78 — мужской гражданский
--                   скин; при регистрации геймод проставляет дефолт по полу
--                   (муж 78 / жен 77). Скин 0 (CJ) недопустим — геймод считает
--                   его невалидным и подменяет мужским дефолтом.
--   phone         — номер телефона (Docs/Phone.md): шестизначный. Телефон ПОКУПАЕТСЯ
--                   в магазине 24/7, автовыдачи нет: NULL — телефона у аккаунта нет.
--                   Повторная покупка меняет номер (перезапись этого же поля).
--                   UNIQUE: по номеру звонят, он обязан быть один на сервере, и
--                   занятость номера ловится именно нарушением этого индекса.
--   phone_balance — счёт телефона: разговор тарифицируется поминутно (см.
--                   Docs/Phone.md), пополняется в 24/7. NOT NULL DEFAULT 0 — счёт
--                   есть всегда, даже пока телефона нет; в минус не уходит.
CREATE TABLE IF NOT EXISTS `player` (
    `id`            BIGINT          NOT NULL AUTO_INCREMENT,
    `name`          VARCHAR(24)     NOT NULL,
    `password_hash` VARCHAR(128)    NOT NULL,
    `sex`           TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `skin`          INT             NOT NULL DEFAULT 78,
    `phone`         BIGINT          NULL DEFAULT NULL,
    `phone_balance` BIGINT          NOT NULL DEFAULT 0,
    `last_seen`     DATETIME        NULL DEFAULT NULL,
    PRIMARY KEY (`id`),
    UNIQUE KEY `uq_name` (`name`),
    UNIQUE KEY `uk_player_phone` (`phone`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;

-- Если таблица `player` уже существует со старой схемой (без личного скина) и
-- дропать её не хочешь — добавь колонку (MySQL НЕ знает IF NOT EXISTS в ADD COLUMN,
-- это синтаксис MariaDB; повтор ответит «Duplicate column name» = уже сделано):
-- ALTER TABLE `player` ADD COLUMN `skin` INT NOT NULL DEFAULT 78;
--
-- Миграция с прежнего дефолта 22 на 78 (для уже существующей колонки) и починка
-- аккаунтов, у которых записан запрещённый теперь скин 0 (появлялись как CJ):
-- ALTER TABLE `player` ALTER COLUMN `skin` SET DEFAULT 78;
-- UPDATE `player` SET `skin` = 78 WHERE `skin` = 0;


-- ============ [2/11] admins.sql ============

-- Схема админ-системы: уровни, пароль админки, регистрация. Баны аккаунтов — в
-- отдельной таблице ban (см. Sql/bans.sql). Применить вручную к схеме геймода.
--
-- account_id      — аккаунт (FK на player.id); одна строка на админа.
-- level           — админ-уровень 0..6 (0 — не админ); гейтит команды только в
--                   связке с /alogin (эффективный уровень = 0 до входа). Уровень
--                   6 («Разработчик») — абсолютный максимум, ставится ТОЛЬКО этим
--                   INSERT (через /setadmin недостижим); он авто-логинится без
--                   пароля, поэтому password_hash для него необязателен.
-- password_hash   — строка-хеш админ-пароля (libsodium crypto_pwhash_str,
--                   crypto_pwhash_STRBYTES = 128, ASCII); NULL — пароль не задан
--                   (для уровней 1..5 даёт «не зарегистрирован», входа нет).
-- registration_ip — IP цели на момент регистрации админки (серверный факт).
-- registered_at   — момент регистрации (NOW() при выдаче пароля).
CREATE TABLE IF NOT EXISTS admin_account (
    account_id      BIGINT       NOT NULL PRIMARY KEY,
    level           TINYINT      NOT NULL DEFAULT 0,
    password_hash   VARCHAR(128) NULL,
    registration_ip VARCHAR(46)  NULL,
    registered_at   TIMESTAMP    NULL DEFAULT NULL,
    CONSTRAINT fk_admin_account FOREIGN KEY (account_id) REFERENCES player(id)
);


-- ============ [3/11] bans.sql ============

-- Баны аккаунтов (проход B админ-системы). Применить вручную к схеме геймода.
--
-- account_id   — забаненный аккаунт (FK на player.id); одна строка на аккаунт,
--                повторный /ban перетирает её (UPSERT).
-- banned_until — момент окончания бана (NOW() + INTERVAL days DAY на записи);
--                бан активен, пока banned_until > NOW() (проверка на логине).
-- reason       — причина из /ban (utf-8, как и прочий хранимый текст; обрезана по
--                байтам без разрыва символа); NULL допустим.
-- banned_by    — аккаунт админа, выдавшего бан (серверный факт); NULL допустим.
-- ip           — IP цели на момент бана (серверный факт), в запас под IP-бан.
-- created_at   — момент выдачи бана.
CREATE TABLE IF NOT EXISTS ban (
    account_id   BIGINT       NOT NULL PRIMARY KEY,
    banned_until DATETIME     NOT NULL,
    reason       VARCHAR(128) NULL,
    banned_by    BIGINT       NULL,
    ip           VARCHAR(46)  NULL,
    created_at   TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP,
    CONSTRAINT fk_ban_account FOREIGN KEY (account_id) REFERENCES player(id)
);


-- ============ [4/11] factions.sql ============

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

-- Персистентный кошелёк заработка в порту (PortWalletService): каждая сданная
-- коробка зачисляется сюда write-through сразу при сдаче; забирается на руки
-- отдельным действием («Забрать деньги» на пикапе порта), не сгорает при
-- дисконнекте/смерти/незавершённой смене.
CREATE TABLE IF NOT EXISTS `port_wallet` (
    `account_id` BIGINT NOT NULL,
    `balance`    BIGINT NOT NULL DEFAULT 0,
    PRIMARY KEY (`account_id`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;

-- Персистентный кошелёк заработка водителя автобуса (BusWalletService): каждый
-- зачтённый чекпоинт маршрута (и бонус за полный круг) зачисляется сюда
-- write-through сразу при зачёте; забирается на руки отдельным действием
-- («Забрать деньги» на пикапе трудоустройства), не сгорает при дисконнекте/
-- смерти/увольнении. Зеркало port_wallet/bank_account (иная таблица).
CREATE TABLE IF NOT EXISTS `bus_wallet` (
    `account_id` BIGINT NOT NULL,
    `balance`    BIGINT NOT NULL DEFAULT 0,
    PRIMARY KEY (`account_id`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;

-- Персистентный кошелёк заработка портового развозчика (HaulerWalletService):
-- каждая РАЗГРУЖЕННАЯ на базе коробка ($100) и бонус за полную разгрузку 10/10
-- ($1000) зачисляются сюда write-through сразу при сдаче; забираются на руки
-- отдельным действием («Забрать деньги» на пикапе трудоустройства), не сгорают
-- при дисконнекте/смерти/увольнении. Зеркало port_wallet/bus_wallet (иная таблица).
CREATE TABLE IF NOT EXISTS `hauler_wallet` (
    `account_id` BIGINT NOT NULL,
    `balance`    BIGINT NOT NULL DEFAULT 0,
    PRIMARY KEY (`account_id`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;

-- Телефон игрока (Docs/Phone.md): ПОКУПАЕТСЯ в магазине 24/7 и живёт с аккаунтом,
-- повторная покупка меняет номер. Для НОВОЙ базы колонка уже есть в CREATE TABLE
-- `player` выше; этот блок — для существующей базы, применять один раз.
--
-- Колонка NULL-able, а НЕ «NOT NULL DEFAULT 0»: под UNIQUE-индексом все старые строки
-- получили бы одинаковый 0 и индекс просто не создался бы (duplicate entry). NULL под
-- UNIQUE разрешён в любом количестве — это и есть «телефона нет».
--
-- MySQL НЕ поддерживает IF NOT EXISTS в ADD COLUMN/ADD INDEX (это синтаксис MariaDB),
-- а голый ALTER на уже применённой базе падает с «Duplicate column/key name» и
-- ОБРЫВАЕТ прогон файла — всё, что ниже, остаётся несозданным. Поэтому проверяем
-- наличие по information_schema и выполняем ALTER динамически: файл целиком
-- идемпотентен и его можно гонять повторно.
SET @has_phone := (SELECT COUNT(*) FROM information_schema.COLUMNS
                   WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'player' AND COLUMN_NAME = 'phone');
SET @sql := IF(@has_phone > 0, 'DO 0', 'ALTER TABLE `player` ADD COLUMN `phone` BIGINT NULL DEFAULT NULL');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @has_phone_idx := (SELECT COUNT(*) FROM information_schema.STATISTICS
                       WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'player'
                         AND INDEX_NAME = 'uk_player_phone');
SET @sql := IF(@has_phone_idx > 0, 'DO 0',
               'ALTER TABLE `player` ADD UNIQUE INDEX `uk_player_phone` (`phone`)');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @has_phone_balance := (SELECT COUNT(*) FROM information_schema.COLUMNS
                           WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'player'
                             AND COLUMN_NAME = 'phone_balance');
SET @sql := IF(@has_phone_balance > 0, 'DO 0',
               'ALTER TABLE `player` ADD COLUMN `phone_balance` BIGINT NOT NULL DEFAULT 0');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @has_last_seen := (SELECT COUNT(*) FROM information_schema.COLUMNS
                       WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'player'
                         AND COLUMN_NAME = 'last_seen');
SET @sql := IF(@has_last_seen > 0, 'DO 0',
               'ALTER TABLE `player` ADD COLUMN `last_seen` DATETIME NULL DEFAULT NULL');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

-- Друзья (Docs/Friends.md). Дружба ВЗАИМНАЯ и хранится ДВУМЯ строками (a->b и b->a):
-- так список друзей аккаунта — это обычный SELECT по account_id без OR и UNION, а
-- удаление снимает обе строки одним DELETE. FK на player с каскадом: удалённый
-- аккаунт не оставляет висячих дружб.
CREATE TABLE IF NOT EXISTS `player_friend` (
    `account_id` BIGINT   NOT NULL,
    `friend_id`  BIGINT   NOT NULL,
    `since`      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`account_id`, `friend_id`),
    CONSTRAINT `fk_friend_account` FOREIGN KEY (`account_id`) REFERENCES `player` (`id`) ON DELETE CASCADE,
    CONSTRAINT `fk_friend_friend` FOREIGN KEY (`friend_id`) REFERENCES `player` (`id`) ON DELETE CASCADE
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;

-- Кошелёк заработка врача (работа-врач, Docs/MedicJob.md): write-through, забирается
-- на пикапе больницы. Как bus_wallet/port_wallet/hauler_wallet.
CREATE TABLE IF NOT EXISTS `medic_wallet` (
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


-- ============ [5/11] families.sql ============

-- Схема бизнес-фичи «Семьи». Применить вручную к схеме геймода.
-- Семьи — player-created группы: создаются игроками в рантайме, в БД лежат
-- целиком (write-through, как членство фракций) и переживают рестарт.
-- Текст хранится в utf-8 (геймод конвертирует cp1251 <-> utf8 сам).

-- id семьи генерирует сервер (единственный писатель), не AUTO_INCREMENT:
-- m_nextId = max(id)+1 на загрузке, INSERT с явным id. name уникально по
-- серверу БЕЗ УЧЁТА РЕГИСТРА ('vlasud' занял — 'Vlasud'/'vlaSuD' невозможны).
-- owner_account_id — лидер. created_at/joined_at — unix-время.
-- Название — одно слово ЛАТИНИЦЕЙ (валидация в FamilyService::validateName),
-- поэтому коллация name — utf8mb4_general_ci: БД-UNIQUE сворачивает регистр так
-- же, как in-memory проверка (equalsIgnoreCaseAscii) — страховочная сетка БД
-- совпадает с правилом. Прежняя bin-коллация была байт-точной и пропустила бы
-- регистровый дубль мимо памяти. Существующей БД нужен одноразовый ALTER:
--   ALTER TABLE family MODIFY name VARCHAR(64) NOT NULL COLLATE utf8mb4_general_ci;
CREATE TABLE IF NOT EXISTS `family` (
    `id`               INT         NOT NULL,
    `name`             VARCHAR(64) NOT NULL COLLATE utf8mb4_general_ci,
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


-- ============ [6/11] house_owner.sql ============

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


-- ============ [7/11] items.sql ============

-- Схема базовой системы ВЕЩЕЙ игрока. Применить вручную к схеме геймода.
-- Сами типы предметов — часть кода (registerItem с фиксированным item_type), в
-- БД только данные: количество предмета на аккаунт.
--
-- Одна строка на пару (аккаунт, тип предмета). Отсутствие строки = у аккаунта
-- предмета нет. Хранятся ТОЛЬКО ненулевые количества (на конце сессии геймод
-- переписывает строки аккаунта снимком: пропавшее удаляется, нули не пишутся).
--
--   account_id — аккаунт (player.id; он же account_id в остальных таблицах:
--                faction_member, bank_account, player_weapon_skill, ...).
--   item_type  — код типа предмета (кодовый реестр InventoryService, > 0).
--                Напр. 1 — аптечка. Незарегистрированный тип геймод отбрасывает
--                при загрузке (мусор/устаревший тип не осядет).
--   quantity   — количество (> 0). Геймод клампит к актуальному maxStack типа при
--                загрузке, так что «лишнее» из БД не превысит потолок стека.
CREATE TABLE IF NOT EXISTS `player_items` (
    `account_id` BIGINT NOT NULL,
    `item_type`  INT    NOT NULL,
    `quantity`   INT    NOT NULL DEFAULT 0,
    PRIMARY KEY (`account_id`, `item_type`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;


-- ============ [8/11] weapon_proficiency.sql ============

-- Схема кастомного скилла ВЛАДЕНИЯ оружием (weapon proficiency). Применить
-- вручную к схеме геймода. Это НЕ нативный SA-MP weapon skill (тот в игре, не в
-- БД) — отдельный стат прогрессии аккаунта 0..100, нарабатываемый стрельбой.
--
-- Одна строка на пару (аккаунт, оружие). Отсутствие строки = скилл 0 (геймод
-- так и грузит: нет строки — слот остаётся нулём). Учитываются ровно 5 оружий
-- (серверные weapon id): 24 дигл, 25 дробовик, 30 AK-47, 31 M4, 34 снайперка.
--
--   account_id — аккаунт (player.id).
--   weapon     — серверный id оружия (одно из 5 учитываемых).
--   skill      — владение 0..100. Каждые 5 валидных выстрелов из оружия → +1
--                (кап 100). Геймод клампит значение при загрузке, так что
--                «лишнее» из БД не превысит 100.
CREATE TABLE IF NOT EXISTS `player_weapon_skill` (
    `account_id` BIGINT NOT NULL,
    `weapon`     INT    NOT NULL,
    `skill`      INT    NOT NULL DEFAULT 0,
    PRIMARY KEY (`account_id`, `weapon`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;


-- ============ [9/11] personal_vehicle.sql ============

-- Владение личным транспортом. Применить вручную к схеме геймода.
-- Личная машина — собственность АККАУНТА: в БД хранится только ПРАВО ВЛАДЕНИЯ
-- (какими моделями владеет аккаунт), грузится на старте сессии и переживает
-- перезаход. Сама машина-СУЩНОСТЬ в мире сессионная — спавнится/уничтожается
-- через парковку (ParkingSystem), её позиция/состояние НЕ сохраняются.
--
-- Запись владения — write-through (как членство фракций faction_member): покупка
-- INSERT'ит строку сразу, загрузка на старте сессии SELECT'ит все строки аккаунта.
--
-- id — суррогатный ключ строки (AUTO_INCREMENT): задел под будущее per-vehicle
-- (продажа/тюнинг конкретной машины); в in-memory владении пока не используется.
-- account_id НЕ уникален: один аккаунт может владеть несколькими машинами
-- (лимит > 1 — будущее), поэтому только индекс idx_account для загрузки.
-- fuel — ПЕРСИСТЕНТНЫЙ остаток бака (0..VehicleService::FUEL_CAPACITY, DOUBLE как и
-- координаты parked_vehicle — SA-MP float проходит round-trip без потерь). DEFAULT
-- 100 = полный бак (только что купленная машина заводится полной). Снимается перед
-- каждым исчезновением заспавненного экземпляра (пере-спавн на парковке, дисконнект
-- владельца, санкционированная смерть) и восстанавливается при появлении — закрывает
-- «докатал бак -> убрал в гараж/детонация -> бесплатный полный бак» (см.
-- Docs/GameDesign/Economy.md «Задел на будущий сток: топливо и заправки»).
--
-- colour1/colour2 — ПЕРСИСТЕНТНЫЙ цвет машины (индекс палитры SA 0..255). DEFAULT -1
-- = «не сохранено» (только что куплена/не загружено) — spawn() спавнит рандомным
-- цветом ядра. Пара неразделима: сохраняется/сбрасывается ВМЕСТЕ (colour1 < 0 —
-- colour2 тоже трактуется как «не задан»).
-- paintjob — ПЕРСИСТЕНТНЫЙ пейнтджоб (0..2 варианта на модель SA). DEFAULT -1 = «нет
-- пейнтджоба».
-- components — ПЕРСИСТЕНТНЫЙ набор компонентов (JSON-массив id, напр. `[1010,1073]`).
-- JSON-колонка (не дочерняя таблица): снимок читается/пишется ЦЕЛИКОМ одним
-- write-through UPDATE вместе с fuel/цветом/пейнтджобом (см. Docs/PersonalVehicle.md
-- «Персист внешнего вида») — нет нужды в join/multi-row транзакции ради набора из
-- максимум 16 элементов. DEFAULT ('') (не NULL; TEXT-DEFAULT требует MySQL 8.0.13+,
-- как и остальная схема) — пустой набор для старых строк без миграции, парсер
-- (componentsFromJson) трактует пустую строку как пустой массив.
--
-- Снимок (fuel+цвет+пейнтджоб+компоненты) снимается с ЖИВОГО экземпляра ПЕРЕД каждым
-- исчезновением заспавненной машины (пере-спавн, конец сессии, санкционированная
-- смерть) И на КАЖДОМ ПРИНЯТОМ изменении тюнинга (мод-шоп/Pay'n'Spray) — переживает
-- краш сервера до штатного деспавна. Применяется ОДИН РАЗ, в spawn().
CREATE TABLE IF NOT EXISTS `personal_vehicle` (
    `id`         BIGINT NOT NULL AUTO_INCREMENT,
    `account_id` BIGINT NOT NULL,
    `model`      INT    NOT NULL,
    `fuel`       DOUBLE NOT NULL DEFAULT 100,
    `colour1`    INT    NOT NULL DEFAULT -1,
    `colour2`    INT    NOT NULL DEFAULT -1,
    `paintjob`   INT    NOT NULL DEFAULT -1,
    `components` TEXT   NOT NULL DEFAULT (''),
    PRIMARY KEY (`id`),
    INDEX `idx_account` (`account_id`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;

-- Миграция для уже существующей таблицы (MySQL 8.0.29+, идемпотентно):
-- ALTER TABLE `personal_vehicle` ADD COLUMN IF NOT EXISTS `fuel` DOUBLE NOT NULL DEFAULT 100;
-- ALTER TABLE `personal_vehicle` ADD COLUMN IF NOT EXISTS `colour1` INT NOT NULL DEFAULT -1;
-- ALTER TABLE `personal_vehicle` ADD COLUMN IF NOT EXISTS `colour2` INT NOT NULL DEFAULT -1;
-- ALTER TABLE `personal_vehicle` ADD COLUMN IF NOT EXISTS `paintjob` INT NOT NULL DEFAULT -1;
-- ALTER TABLE `personal_vehicle` ADD COLUMN IF NOT EXISTS `components` TEXT NOT NULL DEFAULT ('');


-- ============ [10/11] parked_vehicle.sql ============

-- Припаркованные у дома машины. Применить вручную к схеме геймода (заменяет
-- family_vehicle.sql — тот НЕ применять).
-- Парковка — БАЗОВОЕ состояние личной машины у дома владельца: собственность остаётся
-- у аккаунта (строка personal_vehicle НЕ трогается), парковка — это ПОСТОЯННАЯ ТОЧКА
-- СПАВНА у дома + ЗАМОК (водит только владелец). Шеринг семье — РЕЖИМ ДОСТУПА поверх
-- той же припаркованной машины (family_id != -1: водят все члены семьи). В БД
-- write-through: припарковать INSERT'ит строку, убрать DELETE'ит; расшарить/забрать —
-- лишь UPDATE family_id (машина не пересоздаётся). На старте все строки грузятся
-- СТРОГО после семей и машины спавнятся у дома.
--
-- personal_vehicle_id — ссылка на personal_vehicle.id (PK: одна парковка на машину).
-- owner_account_id — аккаунт владельца (личный замок + возврат при выходе из семьи).
-- model — модель машины (дубль personal_vehicle.model — чтобы спавнить, не джойня).
-- x/y/z/angle — АБСОЛЮТНАЯ точка спавна у дома (переживает потерю дома владельцем:
-- существующая парковка не ломается, только НОВАЯ парковка/шеринг требует дом).
-- family_id — -1 (по умолчанию) = личная owner-only; иначе id семьи (расшарена).
--
-- Тип DOUBLE для координат: SA-MP float проходит round-trip через double без потерь
-- (первая таблица со свободными мировыми координатами; player_spawn хранит только
-- индекс выбора, дома — координаты в houses.json). family_id держим INT NOT NULL
-- DEFAULT -1 (а не NULL): -1 == FamilyService::NO_FAMILY — ближе к in-memory модели,
-- убирает nullable-биндинг при чтении/UPDATE.
--
-- Крайние случаи: убрать с парковки -> DELETE по personal_vehicle_id; роспуск семьи /
-- выход владельца / забрать через /family -> UPDATE family_id = -1 (машина остаётся
-- припаркована ЛИЧНО). Расшаренную с несуществующей семьёй старт НЕ теряет —
-- деградирует в личную (family_id трактуется как -1).
--
-- fuel — ПЕРСИСТЕНТНЫЙ остаток бака (0..VehicleService::FUEL_CAPACITY), DEFAULT 100
-- = полный бак (первая парковка INSERT'ит РЕАЛЬНЫЙ остаток на момент парковки, не
-- дефолт — DEFAULT здесь лишь страховка схемы). Снимается перед деспавном экземпляра
-- (выход владельца оффлайн) и восстанавливается при спавне (вход владельца/старт
-- сервера для расшаренных) — тот же принцип, что personal_vehicle.fuel (см.
-- Docs/GameDesign/Economy.md «Задел на будущий сток: топливо и заправки»).
CREATE TABLE IF NOT EXISTS `parked_vehicle` (
    `personal_vehicle_id` BIGINT NOT NULL,
    `owner_account_id`    BIGINT NOT NULL,
    `model`               INT    NOT NULL,
    `x`                   DOUBLE NOT NULL,
    `y`                   DOUBLE NOT NULL,
    `z`                   DOUBLE NOT NULL,
    `angle`               DOUBLE NOT NULL,
    `family_id`           INT    NOT NULL DEFAULT -1,
    `fuel`                DOUBLE NOT NULL DEFAULT 100,
    PRIMARY KEY (`personal_vehicle_id`),
    INDEX `idx_owner` (`owner_account_id`),
    INDEX `idx_family` (`family_id`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;

-- Миграция для уже существующей таблицы (MySQL 8.0.29+, идемпотентно):
-- ALTER TABLE `parked_vehicle` ADD COLUMN IF NOT EXISTS `fuel` DOUBLE NOT NULL DEFAULT 100;


-- ============ [11/11] player_spawn.sql ============

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


-- ============ player_money.sql ============

-- Наличные аккаунта (персист PlayerMoneyService). Применить вручную к схеме
-- геймода. НЕ банковский счёт (см. bank_account) — это баланс на руках,
-- отображаемый в HUD. Один снимок на аккаунт (PK): геймод переписывает `cash`
-- идемпотентным UPSERT на save-канале сессии (конец сессии И периодический
-- автосейв — см. Docs/Autosave.md), НЕ на каждую транзакцию (наличные меняются
-- часто). Нет строки — новый аккаунт, наличные $0 (стартовой эмиссии по входу
-- больше нет — см. Docs/Persistence.md).
--
-- account_id — аккаунт (player.id). cash — серверный баланс наличных
-- (PlayerMoneyService::getMoney), unsigned по природе — отрицательного не бывает.
CREATE TABLE IF NOT EXISTS `player_money` (
    `account_id` BIGINT NOT NULL,
    `cash`       BIGINT NOT NULL DEFAULT 0,
    PRIMARY KEY (`account_id`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;


-- ============ player_weapon.sql ============

-- Оружие аккаунта (персист PlayerWeaponService). Применить вручную к схеме
-- геймода. Одна строка на пару (аккаунт, оружие) — снимок ЖИВОГО инвентаря на
-- момент сохранения (как player_items): геймод REPLACE'ит строки аккаунта
-- (DELETE+INSERT в одной транзакции) на save-канале сессии.
--
-- account_id — аккаунт (player.id). weapon — id оружия (SA-MP weapon id).
-- ammo — патроны на момент сохранения (>= 0).
--
-- КАВЕАТ: GTA теряет оружие на смерти — серверный инвентарь чистится на каждом
-- спавне и восстанавливается ТОЛЬКО на логин-спавне (см. Docs/Persistence.md).
-- Снимок отражает то, что у игрока в руках прямо сейчас, а не исторический
-- максимум — если игрок умер/безоружен на момент автосейва, в БД уйдёт пустой
-- набор.
CREATE TABLE IF NOT EXISTS `player_weapon` (
    `account_id` BIGINT NOT NULL,
    `weapon`     INT    NOT NULL,
    `ammo`       INT    NOT NULL DEFAULT 0,
    PRIMARY KEY (`account_id`, `weapon`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;


-- ============ anticheat_profile.sql ============

-- Личный множитель анти-чита (AntiCheatSystem). Применить вручную к схеме
-- геймода. Одна строка на аккаунт (PK): множитель — ЕДИНСТВЕННОЕ, что анти-чит
-- хранит между сессиями. Сам счёт нарушений живёт только в памяти сессии и
-- обнуляется при выходе.
--
-- multiplier — во сколько раз быстрее этот игрок набирает счёт до отключения:
-- 1.0 у обычного игрока, у пойманного выше (значение задаёт
-- AntiCheatService::kickedMultiplier, по умолчанию 1.2). Пишется write-through в
-- момент назначения кика, UPSERT'ом с GREATEST — понизить множитель случайной
-- перезаписью нельзя, только правкой БД вручную.
--
-- kicks — сколько раз анти-чит отключал этот аккаунт (счётчик для разбора
-- жалоб). updated_at — время последнего кика.
CREATE TABLE IF NOT EXISTS `anticheat_profile` (
    `account_id` BIGINT   NOT NULL,
    `multiplier` FLOAT    NOT NULL DEFAULT 1.0,
    `kicks`      INT      NOT NULL DEFAULT 0,
    `updated_at` DATETIME NULL,
    PRIMARY KEY (`account_id`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;


-- ============ business_owner.sql ============

-- Владение бизнесами. Применить вручную к схеме геймода.
-- Бизнесы (id, тип, интерьер, вход/выход, цена, копилка, ставки аукциона) —
-- контент и торги, лежат в businesses.json. А вот «кто владеет бизнесом» —
-- динамические данные АККАУНТА: в БД write-through (как владение домами
-- house_owner), грузится на старте и применяется к уже заспавненным точкам.
--
-- business_id — бизнес из businesses.json (PK: один владелец на бизнес).
-- account_id — аккаунт владельца (UNIQUE: ОДИН БИЗНЕС НА АККАУНТ, БД-бэкстоп к
-- in-memory проверке ownsBusiness). claimed_at — unix-время получения бизнеса
-- (диагностика). Запись владения — DELETE+INSERT в одной throwQuery (атомарно по
-- business_id и account_id), снос бизнеса стирает строку владения (нет
-- осиротевшего владения).
CREATE TABLE IF NOT EXISTS `business_owner` (
    `business_id` INT    NOT NULL,
    `account_id`  BIGINT NOT NULL,
    `claimed_at`  BIGINT NOT NULL DEFAULT 0,
    PRIMARY KEY (`business_id`),
    UNIQUE KEY `uq_account` (`account_id`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;


-- Выручка бизнеса ПО ДНЯМ — для окна «Прибыль» в /business. Копилка (balance)
-- отвечает на «сколько лежит сейчас», а владельцу нужно ещё «сколько принесла точка
-- за неделю»; из копилки это не восстановить — её обнуляет каждое снятие.
--
-- amount правится ТОЛЬКО относительно (amount = amount + ?): порядок задач в пуле
-- БД не определён, а сложение коммутативно, поэтому ключ упорядочивания не нужен.
-- Дату ставит БД (CURDATE), чтобы запись и выборка не разъезжались по часовым поясам.
CREATE TABLE IF NOT EXISTS `business_income` (
    `business_id` INT    NOT NULL,
    `day`         DATE   NOT NULL,
    `amount`      BIGINT NOT NULL DEFAULT 0,
    PRIMARY KEY (`business_id`, `day`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;


-- СКЛАД точки: сколько единиц товара лежит и ждёт покупателя. Продажа списывает
-- единицу, заказ владельца («Заказать товар» в /business) пополняет. Пустой склад
-- продавать отказывается.
--
-- Запись АБСОЛЮТНАЯ и под ключом упорядочивания (business_stock:<id>:<item>).
-- Относительная дельта тут не годится: у остатка есть ПОТОЛОК, клампится он в
-- памяти, и потерянная дельта увела бы БД за границу.
CREATE TABLE IF NOT EXISTS `business_stock` (
    `business_id` INT NOT NULL,
    `item_type`   INT NOT NULL,
    `quantity`    INT NOT NULL DEFAULT 0,
    PRIMARY KEY (`business_id`, `item_type`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;


-- ЗАКАЗЫ товара для бизнесов. Хранят УЖЕ СПИСАННЫЕ у владельца деньги (закупку и
-- премию развозчикам), поэтому обязаны пережить рестарт: потерянный заказ — это
-- потерянные деньги игрока.
--
-- items — состав заказа в JSON ([{"item":1,"qty":20}, ...]). Отдельной таблицей не
-- делаем: состав всегда читается и пишется целиком вместе с заказом, а join ради
-- двух строк только добавил бы путей рассинхрона.
--
-- boxes_done — прогресс в коробках (0..10). Срыв доставки прогресс НЕ откатывает:
-- довезённое уже лежит на складе точки, и повторно его везти нельзя.
-- driver_id в БД НЕТ: кто везёт — рантайм, после рестарта заказ просто в пуле.
CREATE TABLE IF NOT EXISTS `business_order` (
    `id`          INT    NOT NULL AUTO_INCREMENT,
    `business_id` INT    NOT NULL,
    `bonus`       BIGINT NOT NULL DEFAULT 0,
    `boxes_done`  INT    NOT NULL DEFAULT 0,
    `items`       TEXT   NOT NULL,
    `created_at`  BIGINT NOT NULL DEFAULT 0,
    PRIMARY KEY (`id`),
    KEY `idx_business` (`business_id`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;


-- ============ auction.sql ============

-- Аукционы (AuctionService). Применить вручную к схеме геймода.
-- Торги — ДИНАМИКА: деньги за ставку списываются в момент ставки, поэтому ставки
-- обязаны переживать рестарт и живут только здесь. json-файлы фич (houses.json,
-- businesses.json) — дев-КОНТЕНТ (описание точек), в проде они read-only.
--
-- category — СТАБИЛЬНЫЙ строковый ключ категории ("business", "house"), а не её
-- индекс регистрации: порядок систем правится, а ставки обязаны оставаться на
-- своих лотах. lot_id — id объекта внутри своей фичи (дом/бизнес).

-- Срок торгов по лоту. Стартует с ПЕРВОЙ ставки, хранится абсолютным unix-временем
-- (не таймером в памяти): отсчёт обязан идти и пока сервер лежит. Строка живёт
-- ровно пока по лоту есть ставки — снимается вместе с последней.
CREATE TABLE IF NOT EXISTS `auction_lot` (
    `category` VARCHAR(32) NOT NULL,
    `lot_id`   INT         NOT NULL,
    `ends_at`  BIGINT      NOT NULL DEFAULT 0,
    PRIMARY KEY (`category`, `lot_id`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;

-- Ставки. Одна на аккаунт в пределах лота (PK): повышение ставки — UPDATE суммы,
-- а не вторая строка. Снятие ставки/лота — DELETE; лот и его ставки снимаются
-- ОДНОЙ транзакцией (лот без срока не дозреет, срок без ставок даст пустой итог).
CREATE TABLE IF NOT EXISTS `auction_bid` (
    `category`   VARCHAR(32) NOT NULL,
    `lot_id`     INT         NOT NULL,
    `account_id` BIGINT      NOT NULL,
    `amount`     BIGINT      NOT NULL,
    PRIMARY KEY (`category`, `lot_id`, `account_id`),
    KEY `idx_account` (`account_id`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;

-- Долги аукциона перед аккаунтом: невыданный возврат проигравшей ставки (игрок был
-- офлайн в момент итогов — наличные сессионные, вручить некому) и однократная
-- пометка «твою ставку перебили». Одна строка на аккаунт (PK). Обнулившаяся строка
-- НЕ удаляется: DELETE, переставший в пуле местами с начислением возврата, стёр бы
-- деньги. refund правится только ОТНОСИТЕЛЬНО (refund = refund + дельта, без клампа)
-- — перестановка задач должна быть безразлична. Отрицательное значение поэтому
-- допустимо и промежуточно; на загрузке строка берётся только при refund > 0.
CREATE TABLE IF NOT EXISTS `auction_player` (
    `account_id` BIGINT  NOT NULL,
    `refund`     BIGINT  NOT NULL DEFAULT 0,
    `outbid`     TINYINT NOT NULL DEFAULT 0,
    PRIMARY KEY (`account_id`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;


-- ============ player_return_point.sql ============

-- Точка возврата аккаунта (ReturnPointSystem, см. Docs/ReturnPoint.md). Применить
-- вручную к схеме геймода. Одна строка на аккаунт (PK) — снимок ПОСЛЕДНЕГО
-- местонахождения игрока, переписывается идемпотентным UPSERT на save-канале сессии
-- (конец сессии И периодический автосейв, см. Docs/Autosave.md). Нет строки — новый
-- аккаунт/первый вход: возвращаться некуда, диалог на логин-спавне не показывается.
--
-- x/y/z — ПРИНЯТАЯ СЕРВЕРОМ позиция (PlayerLocationService), не сырая клиентская.
-- angle — yaw модели в градусах на момент снимка; у игрока В ТРАНСПОРТЕ поворот
-- модели не обновляется (vehicle-синк пишет только позицию), поэтому для логаута за
-- рулём здесь лежит последний ПЕШИЙ курс — координаты при этом верные.
-- interior — интерьер 0..255; virtual_world — виртуальный мир (серверный факт).
-- Транспорт не сохраняется: вышел за рулём — вернётся пешком на те же координаты.
--
-- Тип DOUBLE для координат/угла — как parked_vehicle: SA-MP float проходит round-trip
-- через double без потерь. Битую строку (нефинитные/запредельные значения, интерьер
-- вне 0..255, отрицательный virtual_world — мир без пикапов выхода) геймод при
-- загрузке ОТБРАСЫВАЕТ и считает, что точки нет; такой же снимок он симметрично НЕ
-- пишет — одно правило годности на запись и чтение.
CREATE TABLE IF NOT EXISTS `player_return_point` (
    `account_id`    BIGINT NOT NULL,
    `x`             DOUBLE NOT NULL,
    `y`             DOUBLE NOT NULL,
    `z`             DOUBLE NOT NULL,
    `angle`         DOUBLE NOT NULL DEFAULT 0,
    `interior`      INT    NOT NULL DEFAULT 0,
    `virtual_world` INT    NOT NULL DEFAULT 0,
    PRIMARY KEY (`account_id`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;
