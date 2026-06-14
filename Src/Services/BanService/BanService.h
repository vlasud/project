#pragma once

#include "Services/IService.h"
#include <cstdint>
#include <string>

// Источник правды о банах аккаунтов. Бан хранится ТОЛЬКО в БД (таблица ban):
// забаненный по определению не онлайн, поэтому RAM-кэш не нужен. Проверка
// активности бана — на ЛОГИНЕ (PlayerAuthSystem), до выдачи доступа.
//
// Модель: бан НА ДНИ (banned_until = NOW() + INTERVAL days DAY). IP цели
// пишется в запас (на будущий IP-бан), на проверку логина пока не влияет.
// Запись — write-through одной строкой через DatabaseManager::throwQuery
// (параметризованный UPSERT, без конкатенации).
class BanService final : public IService
{
  public:
    // Забанить аккаунт на days дней (write-through UPSERT в ban). Все параметры —
    // СЕРВЕРНЫЕ факты (accountId/byAccountId из сессий, ip из networkData), days
    // провалидирован вызывающим. Передаются по значению — задача уходит на
    // воркер, mysqlx-объекты границу потока не пересекают (контракт).
    void banAccount(std::int64_t accountId, int days, std::string reason, std::int64_t byAccountId, std::string ip);
};
