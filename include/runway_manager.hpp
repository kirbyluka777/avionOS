#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

struct RunwayResult {
    std::string flightCode;
    int runwayId{};
    int effectivePriority{};
};

/**
 * @class RunwayManager
 * @brief Administra la asignación concurrente de pistas aeroportuarias.
 *
 * Controla el acceso exclusivo a las pistas de aterrizaje y despegue
 * del Aeropuerto Internacional Manuel Carlos Piar.
 * 
 * Implementa mecanismos de sincronización mediante:
 * 
 * - `std::mutex`: protege el estado compartido de las pistas.
 * 
 * - `std::unique_lock`: permite controlar el bloqueo y desbloqueo.
 * 
 * - `std::condition_variable`: suspende los hilos mientras esperan
 *   una pista disponible.
 *
 * @note Las pistas se identifican mediante números enteros consecutivos.
 * @note La prioridad efectiva más alta tiene preferencia.
 */
class RunwayManager {
public:
    explicit RunwayManager(int runwayCount,
                           std::chrono::milliseconds agingInterval = std::chrono::seconds(2))
        : agingInterval_(agingInterval) {
        if (runwayCount <= 0) {
            throw std::invalid_argument("Debe existir al menos una pista");
        }
        if (agingInterval_.count() <= 0) {
            throw std::invalid_argument("El intervalo de envejecimiento debe ser positivo");
        }
        for (int i = 1; i <= runwayCount; ++i) {
            runways_.push_back(Runway{i, false, {}});
        }
    }

    RunwayResult useRunway(const std::string& flightCode,
                           int basePriority,
                           std::chrono::milliseconds operationTime) {
        if (operationTime.count() < 0) {
            throw std::invalid_argument("El tiempo de uso de pista no puede ser negativo");
        }
        const auto requestTime = Clock::now();
        std::unique_lock<std::mutex> lock(mutex_);
        const std::size_t ticket = nextTicket_++;
        waiting_.push_back(Waiter{ticket, flightCode, basePriority, requestTime});

        condition_.wait(lock, [&] {
            const auto best = bestWaiterLocked(Clock::now());
            return best.has_value() && best.value() == ticket && hasFreeRunwayLocked();
        });

        auto runwayIt = std::find_if(runways_.begin(), runways_.end(),
                                     [](const Runway& runway) { return !runway.occupied; });
        if (runwayIt == runways_.end()) {
            throw std::logic_error("Invariante violada: no hay pista libre tras la notificación");
        }

        const int assignedId = runwayIt->id;
        runwayIt->occupied = true;
        runwayIt->flightCode = flightCode;
        const auto waiterIt = std::find_if(waiting_.begin(), waiting_.end(),
                                           [ticket](const Waiter& waiter) {
                                               return waiter.ticket == ticket;
                                           });
        const int effectivePriority = effectivePriorityLocked(*waiterIt, Clock::now());
        waiting_.erase(waiterIt);
        condition_.notify_all();
        lock.unlock();

        std::this_thread::sleep_for(operationTime);

        lock.lock();
        runwayIt = std::find_if(runways_.begin(), runways_.end(),
                                [assignedId](const Runway& runway) { return runway.id == assignedId; });
        if (runwayIt == runways_.end() || !runwayIt->occupied || runwayIt->flightCode != flightCode) {
            throw std::logic_error("Estado inconsistente al liberar la pista");
        }
        runwayIt->occupied = false;
        runwayIt->flightCode.clear();
        lock.unlock();
        condition_.notify_all();

        return RunwayResult{flightCode, assignedId, effectivePriority};
    }

    std::size_t waitingCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return waiting_.size();
    }

private:
    using Clock = std::chrono::steady_clock;

    struct Runway {
        int id;
        bool occupied;
        std::string flightCode;
    };

    struct Waiter {
        std::size_t ticket;
        std::string flightCode;
        int basePriority;
        Clock::time_point requestedAt;
    };

    int effectivePriorityLocked(const Waiter& waiter, Clock::time_point now) const {
        const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(now - waiter.requestedAt);
        const auto increments = waited.count() / agingInterval_.count();
        return waiter.basePriority + static_cast<int>(increments);
    }

    std::optional<std::size_t> bestWaiterLocked(Clock::time_point now) const {
        if (waiting_.empty()) return std::nullopt;
        const Waiter* best = &waiting_.front();
        for (const auto& candidate : waiting_) {
            const int candidatePriority = effectivePriorityLocked(candidate, now);
            const int bestPriority = effectivePriorityLocked(*best, now);
            if (candidatePriority > bestPriority ||
                (candidatePriority == bestPriority && candidate.ticket < best->ticket)) {
                best = &candidate;
            }
        }
        return best->ticket;
    }

    bool hasFreeRunwayLocked() const {
        return std::any_of(runways_.begin(), runways_.end(),
                           [](const Runway& runway) { return !runway.occupied; });
    }

    std::vector<Runway> runways_;
    std::vector<Waiter> waiting_;
    std::chrono::milliseconds agingInterval_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::size_t nextTicket_{0};
};
