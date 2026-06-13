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
CREATE TABLE IF NOT EXISTS `player` (
    `id`            BIGINT          NOT NULL AUTO_INCREMENT,
    `name`          VARCHAR(24)     NOT NULL,
    `password_hash` VARCHAR(128)    NOT NULL,
    `sex`           TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `skin`          INT             NOT NULL DEFAULT 78,
    PRIMARY KEY (`id`),
    UNIQUE KEY `uq_name` (`name`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;

-- Если таблица `player` уже существует со старой схемой (без личного скина) и
-- дропать её не хочешь — добавь колонку идемпотентно (MySQL 8.0.29+):
-- ALTER TABLE `player` ADD COLUMN IF NOT EXISTS `skin` INT NOT NULL DEFAULT 78;
--
-- Миграция с прежнего дефолта 22 на 78 (для уже существующей колонки) и починка
-- аккаунтов, у которых записан запрещённый теперь скин 0 (появлялись как CJ):
-- ALTER TABLE `player` ALTER COLUMN `skin` SET DEFAULT 78;
-- UPDATE `player` SET `skin` = 78 WHERE `skin` = 0;
