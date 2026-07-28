#pragma once

#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/JobDismissService/JobDismissService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Универсальный выход с работы: /stopjob. Бизнес-фича (НЕ Core).
//
// Раньше прервать смену можно было только у пикапа своей работы — то есть с другого
// конца карты никак, хотя смена держит транспорт, лок навигации и окна анти-AFK.
// Команда работает из ЛЮБОЙ точки и с ЛЮБОЙ работой: какая именно смена идёт, знает
// JobDismissService, а увольняет по-прежнему сама работа своим обработчиком.
//
// Прерывание необратимо и стоит игроку взноса, поэтому команда не увольняет сразу, а
// показывает диалог с названием работы и её предупреждением; на подтверждении факт
// «работает» перепроверяется (за время диалога смена могла кончиться сама).
//
// Новая работа подключается регистрацией в своём конструкторе — эту систему трогать
// не нужно.
class JobDismissSystem : public BaseSystem
{
  public:
    JobDismissSystem(ICore &core, const ServiceRegister &serviceRegister);

  private:
    void onStopJob(IPlayer &player);

    JobDismissService &m_jobDismissService;
    PlayerDialogService &m_dialogService;
};
