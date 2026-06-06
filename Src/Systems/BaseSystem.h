#pragma once

struct ICore;

#include "../Services/ServiceRegister.h"
#include <core.hpp>

class BaseSystem
{
  public:
    explicit BaseSystem(ICore &core, const ServiceRegister &serviceRegister)
        : m_core(core), m_serviceRegister(serviceRegister)
    {
    }

    virtual ~BaseSystem() = default;

    virtual void initialize() = 0;
    virtual void reset() = 0;

  protected:
    ICore &m_core;
    const ServiceRegister &m_serviceRegister;
};
