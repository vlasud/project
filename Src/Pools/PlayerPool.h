#pragma once

#include "../Player/Player.h"
#include "IndexPool.h"

class PlayerPool : public IndexPool<Player, 1000>
{
  public:
    static Player *get(unsigned id)
    {
        return IndexPool::get(id);
    }
};
