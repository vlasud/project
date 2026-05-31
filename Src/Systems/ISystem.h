#pragma once

struct ICore;

class ISystem
{
  public:
    virtual ~ISystem() = default;

    virtual void link(ICore *core) = 0;
    virtual void initialize() = 0;
    virtual void reset() = 0;
};
