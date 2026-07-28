#pragma once

#include "Macro.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/JobWalletService/JobWalletService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include <array>

// Единый доступ к заработку по всем работам: /jobwallet + напоминание на входе.
// Бизнес-фича (НЕ Core).
//
// Заработок работ персистентен и забирается у пикапа своей работы. Игрок, вышедший с
// деньгами в кошельке, про них в следующий раз не вспоминает — поэтому после логина,
// КОГДА КОШЕЛЬКИ УЖЕ ЗАГРУЗИЛИСЬ и там действительно что-то есть, ему уходит одна
// строка в чат. Одна на сессию: кошельки грузятся асинхронно и по одному, напоминание
// шлём на первой загрузке, которая дала ненулевую сумму.
//
// Окно СПРАВОЧНОЕ: деньги отсюда НЕ выдаются. Единственная точка выдачи — пикап самой
// работы, туда надо приехать; список лишь говорит, сколько и где лежит. Показываются
// ВСЕ зарегистрированные работы, в том числе с нулём (правило проекта — недоступность
// не прячет строку).
class JobWalletSystem : public BaseSystem
{
  public:
    JobWalletSystem(ICore &core, const ServiceRegister &serviceRegister);

  private:
    void onWalletLoaded(int playerId); // кошелёк работы догрузился из БД
    void showWallets(IPlayer &player);

    JobWalletService &m_jobWalletService;
    PlayerDialogService &m_dialogService;
    PlayerSessionService &m_sessionService;

    // Напоминание уже отправлено в этой сессии (иначе три загрузки — три строки).
    std::array<bool, MAX_PLAYERS> m_reminded{};
};
