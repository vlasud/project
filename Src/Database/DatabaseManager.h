#pragma once

#include "Log/LogManager.h"
#include "Pools/StaticPool.h"
#include "ThreadPool/ThreadPool.h"
#include <functional>
#include <mysqlx/xdevapi.h>
#include <queue>
#include <string>
#include <thread>
#include <utility>

class SessionWrapper
{
  public:
    bool initialize();
    mysqlx::Schema getSchema();

  private:
    std::unique_ptr<mysqlx::Session> m_session;
};

// Асинхронный доступ к MySQL поверх ThreadPool.
//
// КОНТРАКТ ПОТОКОВ. Connector/C++ (X DevAPI) требует, чтобы Session и любой
// Result использовались максимум ОДНИМ потоком за раз — объект нельзя
// конструировать на одном потоке, а читать/разрушать на другом. Поэтому здесь
// НИ ОДИН mysqlx-объект не пересекает границу потока:
//
//   • throwQuery  — задача целиком выполняется на воркере, ничего не возвращает.
//   • selectQuery — задача выполняется И ПОЛНОСТЬЮ ВЫЧИТЫВАЕТ результат на
//                   воркере, возвращая владеющие данные T (контейнеры/POD).
//                   На главный поток в callback приходит уже готовое T.
//
// Раньше selectQuery отдавал на главный поток живой RowResult и дренажил его там
// — это нарушало контракт и роняло сервер порчей общей кучи (ucrtbase). Не
// возвращайте RowResult/SqlResult/Row из задачи: вычитывайте прямо в ней.
class DatabaseManager
{
    using Task = std::function<void(mysqlx::Schema)>;
    using ErrorCallback = std::function<void(const std::string &)>;

  public:
    static void initialize();

    // errorCallback (опционально) вызывается на главном потоке, если запрос
    // бросил исключение. Сессия в любом случае возвращается в пул, ошибка
    // логируется.
    static void throwQuery(Task task, ErrorCallback errorCallback = {});

    // task — на воркере: выполняет запрос и вычитывает результат в T.
    // callback — на главном потоке: получает готовое T.
    template <typename T>
    static void selectQuery(std::function<T(mysqlx::Schema)> task, std::function<void(T)> callback,
                            ErrorCallback errorCallback = {});

  private:
    struct PendingThrow
    {
        Task task;
        ErrorCallback errorCallback;
    };
    // Бэкпрешер для selectQuery: тип T стёрт — храним замыкание, повторно
    // вызывающее selectQuery<T>, когда освободится сессия.
    struct PendingSelect
    {
        std::function<void()> retry;
    };

    // RAII для занятой сессии: возвращает её в пул при разрушении, если задачу
    // не удалось поставить в очередь (например, ThreadPool::addTask бросил
    // bad_alloc). commit() передаёт владение колбэкам задачи, и тогда релиз
    // делает releaseAndPump. Гарантия: сессия освобождается ровно один раз на
    // ЛЮБОМ пути, утечки слота пула нет.
    class SessionGuard
    {
      public:
        explicit SessionGuard(SessionWrapper *session) : m_session(session)
        {
        }
        SessionGuard(const SessionGuard &) = delete;
        SessionGuard &operator=(const SessionGuard &) = delete;
        ~SessionGuard()
        {
            if (m_session)
            {
                m_sessionPool.release(m_session);
            }
        }
        void commit()
        {
            m_session = nullptr;
        }

      private:
        SessionWrapper *m_session;
    };

    template <typename T>
    static void dispatchSelect(SessionWrapper *sessionWrapper, std::function<T(mysqlx::Schema)> task,
                               std::function<void(T)> callback, ErrorCallback errorCallback);

    static void releaseAndPump(SessionWrapper *sessionWrapper);

    // Весь доступ к пулу/очередям — строго с главного потока (пул и std::queue
    // не синхронизированы; на этом держится и контракт коннектора «одна сессия
    // — один поток»). В debug это ловится assert'ом.
    static void assertMainThread();

    inline static StaticPool<SessionWrapper, 16> m_sessionPool;
    inline static std::queue<PendingThrow> m_queue;
    inline static std::queue<PendingSelect> m_selectQueue;
    inline static std::thread::id s_mainThreadId; // фиксируется в initialize()
};

// ------------------------------------------------------ шаблонные определения

template <typename T>
void DatabaseManager::selectQuery(std::function<T(mysqlx::Schema)> task, std::function<void(T)> callback,
                                  ErrorCallback errorCallback)
{
    assertMainThread();
    SessionWrapper *sessionWrapper = m_sessionPool.get();
    if (sessionWrapper == nullptr)
    {
        m_selectQueue.push({[task = std::move(task), callback = std::move(callback),
                             errorCallback = std::move(errorCallback)]() mutable
                            {
                                selectQuery<T>(std::move(task), std::move(callback), std::move(errorCallback));
                            }});
        return;
    }
    dispatchSelect<T>(sessionWrapper, std::move(task), std::move(callback), std::move(errorCallback));
}

template <typename T>
void DatabaseManager::dispatchSelect(SessionWrapper *sessionWrapper, std::function<T(mysqlx::Schema)> task,
                                     std::function<void(T)> callback, ErrorCallback errorCallback)
{
    // Сессия уже занята: до успешной постановки задачи владеет ею гард —
    // если addTask бросит, сессия вернётся в пул, а не утечёт.
    SessionGuard guard(sessionWrapper);

    ThreadPool::Task<T> asyncTask;

    // func — на воркере: запрос + вычитка результата в T (mysqlx-объекты живут
    // и умирают здесь же).
    asyncTask.func = [task = std::move(task), sessionWrapper]() { return task(sessionWrapper->getSchema()); };

    // callback — на главном потоке: только готовые данные T.
    asyncTask.callback = [callback = std::move(callback), sessionWrapper](T data)
    {
        callback(std::move(data));
        releaseAndPump(sessionWrapper);
    };

    asyncTask.errorCallback = [errorCallback = std::move(errorCallback), sessionWrapper](const std::string &error)
    {
        LogManager::log(Error, "DatabaseManager: select failed: " + error);
        if (errorCallback)
        {
            errorCallback(error);
        }
        releaseAndPump(sessionWrapper);
    };

    ThreadPool::addTask(std::move(asyncTask));
    guard.commit(); // задача принята — релиз теперь делает releaseAndPump в её колбэке
}
