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
