#pragma once

#include "IGameDbRepository.h"

/** 进程内 GameDB 后端指针（类似 GameRequestTransport） */
class GameDbRepository {
public:
    static void Set(IGameDbRepository *repo);
    static IGameDbRepository *Get();
};
