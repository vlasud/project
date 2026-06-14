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
