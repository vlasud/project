#pragma once

#include "Services/ParkedVehicleService/ParkedVehicleService.h"
#include "Utils/Encoding/Encoding.h"
#include <player.hpp>

// Единый ответ игроку на вызов машины к её месту парковки. Входов в вызов два —
// /car «Мои машины» (владелец) и /family «Транспорт семьи» (член семьи) — а
// объяснение одного и того же кода сервиса должно быть одно: иначе два меню
// трактуют один отказ по-разному. Header-only (аналог ParkedVehicleRow).
namespace ParkedVehicleCall
{
// Цвета передаёт вызывающая система (у каждой свои локальные константы сообщений).
// driverId — СНИМОК водителя машины ДО вызова (`-1`, если машины в мире нет или она
// пуста): повторный вызов подаёт машину на место через respawnHome и отказывает под
// сидящим водителем, а отказ обязан различать «за рулём сам нажавший» и «чужой» —
// иначе он читается как баг («какой водитель, я один»).
inline void reply(IPlayer &player, ParkedVehicleService::Result result, Colour info, Colour error,
                  int driverId = -1)
{
    switch (result)
    {
    case ParkedVehicleService::Result::Ok:
        player.sendClientMessage(info, u("Машина вызвана и ждёт на своём месте парковки"));
        break;
    case ParkedVehicleService::Result::Occupied:
        player.sendClientMessage(error, driverId == player.getID()
                                            ? u("Вы сами за рулём — выйдите из машины, чтобы подать её на место")
                                            : u("Пока в машине водитель, подать её на место нельзя"));
        break;
    case ParkedVehicleService::Result::NotParked:
        player.sendClientMessage(error, u("Эта машина не в гараже — её сначала нужно припарковать у дома"));
        break;
    case ParkedVehicleService::Result::NoAccess:
        player.sendClientMessage(error,
                                 u("Вызывать эту машину может только владелец или семья, которой она расшарена"));
        break;
    default:
        player.sendClientMessage(error, u("Сейчас машину вызвать нельзя"));
        break;
    }
}
} // namespace ParkedVehicleCall
