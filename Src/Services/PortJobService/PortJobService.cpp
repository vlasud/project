#include "Services/PortJobService/PortJobService.h"

PortJobService::Phase PortJobService::phaseOf(int playerId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return Phase::NotWorking;
    }
    return m_state[playerId].phase;
}

bool PortJobService::isWorking(int playerId) const
{
    return phaseOf(playerId) != Phase::NotWorking;
}

int PortJobService::assignedSpotOf(int playerId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return -1;
    }
    return m_state[playerId].assignedSpot;
}

int PortJobService::deliveredOf(int playerId) const
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return 0;
    }
    return m_state[playerId].delivered;
}

bool PortJobService::startWork(int playerId)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return false;
    }
    State &state = m_state[playerId];
    if (state.phase != Phase::NotWorking)
    {
        return false;
    }
    state.phase = Phase::GoToSource;
    return true;
}

int PortJobService::assignDropSpot(int playerId)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return -1;
    }
    State &state = m_state[playerId];
    if (state.phase != Phase::GoToSource)
    {
        return -1;
    }

    // Балансировщик: (1) минимальная текущая занятость среди DROP_COUNT — приоритет
    // НЕ нарушается ради разнообразия; (2) среди точек с этим минимумом — первая,
    // не совпадающая с lastSpot этого игрока (маршрут не повторяется два раза
    // подряд, если есть выбор); (3) тай-брейк — меньший индекс. Два прохода по
    // DROP_COUNT (найти минимум, затем выбрать среди min-кандидатов) — по-прежнему
    // O(DROP_COUNT), без аллокаций; холодный путь (по событию входа в чекпоинт
    // источника, не per-tick).
    int minLoad = m_assigned[0];
    for (int i = 1; i < DROP_COUNT; ++i)
    {
        if (m_assigned[i] < minLoad)
        {
            minLoad = m_assigned[i];
        }
    }

    int chosen = -1;
    for (int i = 0; i < DROP_COUNT; ++i)
    {
        if (m_assigned[i] != minLoad)
        {
            continue;
        }
        if (i != state.lastSpot)
        {
            chosen = i; // первый среди мин.-загруженных, отличный от прошлой точки игрока
            break;
        }
        if (chosen < 0)
        {
            chosen = i; // единственный min-кандидат — это и есть lastSpot, берём его
        }
    }

    ++m_assigned[chosen];
    state.assignedSpot = chosen;
    state.lastSpot = chosen;
    state.phase = Phase::Carrying;
    return chosen;
}

bool PortJobService::completeDelivery(int playerId)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return false;
    }
    State &state = m_state[playerId];
    if (state.phase != Phase::Carrying)
    {
        return false;
    }

    releaseSpot(state);
    ++state.delivered;
    state.phase = Phase::GoToSource;
    return true;
}

void PortJobService::dropCarry(int playerId)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return;
    }
    State &state = m_state[playerId];
    if (state.phase != Phase::Carrying)
    {
        return; // не несёт ящик — ронять нечего (GoToSource/NotWorking без изменений)
    }
    releaseSpot(state); // ящик потерян при смерти — освободить слот распределения точки
    state.phase = Phase::GoToSource; // delivered сохраняется: смена продолжается
}

unsigned long long PortJobService::endWork(int playerId)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return 0;
    }
    State &state = m_state[playerId];
    if (state.phase == Phase::NotWorking)
    {
        return 0;
    }

    releaseSpot(state); // увольнение застало в Carrying — освободить слот распределения
    const unsigned long long pay = static_cast<unsigned long long>(state.delivered) * PAY_PER_BOX;
    state = State{};
    return pay;
}

void PortJobService::resetPlayer(int playerId)
{
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return;
    }
    State &state = m_state[playerId];
    releaseSpot(state); // сгоревшая смена — слот распределения не должен утечь
    state = State{};
}

void PortJobService::releaseSpot(State &state)
{
    if (state.assignedSpot < 0)
    {
        return;
    }
    if (state.assignedSpot < DROP_COUNT)
    {
        --m_assigned[state.assignedSpot];
    }
    state.assignedSpot = -1;
}
