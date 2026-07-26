#pragma once

constexpr int MAX_PLAYERS = 1000;

// Валиден ли id игрока как индекс пер-игроковых массивов сервисов.
//
// Сервисы держат состояние в std::array<..., MAX_PLAYERS> и индексируют его
// напрямую. Пока id приходит из IPlayer::getID(), он всегда в диапазоне, но в
// проекте есть функции, штатно возвращающие -1 (VehicleService::getDriver,
// PlayerSessionService::playerByAccount, PersonalVehicleService::currentVehicle).
// Один такой результат, переданный в сервис без проверки, — запись мимо массива.
// Поэтому ЛЮБОЙ публичный метод сервиса, принимающий playerId, начинается с этой
// проверки: геттер возвращает дефолт, изменяющий метод выходит.
constexpr bool validPlayerId(int playerId) noexcept
{
    return playerId >= 0 && playerId < MAX_PLAYERS;
}
