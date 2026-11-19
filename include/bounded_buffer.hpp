#pragma once

#include <array>
#include <cstddef>
#include <mutex>
#include <semaphore>
#include <stdexcept>
#include <utility>

/**
 * @class BoundedBuffer
 * @brief Implementa un buffer circular de capacidad limitada.
 * 
 * Esta clase administra una estructura de almacenamiento temporal
 * compartida entre uno o varios productores y consumidores.
 * Utiliza dos semáforos contadores para controlar los espacios
 * disponibles y los elementos almacenados, además de un mutex
 * para proteger el acceso concurrente al arreglo interno.
 *
 * @tparam T Tipo de dato que almacenará el buffer.
 * @tparam Capacity Capacidad máxima del buffer circular.
 */
template <typename T, std::size_t Capacity>
class BoundedBuffer {
    static_assert(Capacity > 0, "La capacidad del buffer debe ser mayor que cero");

public:
    BoundedBuffer() : freeSlots_(static_cast<std::ptrdiff_t>(Capacity)),
                      usedSlots_(0) {}

    BoundedBuffer(const BoundedBuffer&) = delete;
    BoundedBuffer& operator=(const BoundedBuffer&) = delete;

    void push(T value) {
        freeSlots_.acquire();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            buffer_[tail_] = std::move(value);
            tail_ = (tail_ + 1) % Capacity;
            ++count_;
        }
        usedSlots_.release();
    }

    T pop() {
        usedSlots_.acquire();
        T value;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            value = std::move(buffer_[head_]);
            head_ = (head_ + 1) % Capacity;
            --count_;
        }
        freeSlots_.release();
        return value;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return count_;
    }

    static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    std::array<T, Capacity> buffer_{};
    std::size_t head_{0};
    std::size_t tail_{0};
    std::size_t count_{0};
    mutable std::mutex mutex_;
    std::counting_semaphore<static_cast<std::ptrdiff_t>(Capacity)> freeSlots_;
    std::counting_semaphore<static_cast<std::ptrdiff_t>(Capacity)> usedSlots_;
};
