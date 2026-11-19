#include "bounded_buffer.hpp"
#include "memory_manager.hpp"
#include "runway_manager.hpp"

#include <atomic>
#include <stdexcept>
#include <string>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("Prueba fallida: " + message);
}

int main() {
    // Prueba 1: el buffer finito conserva todos los elementos y su orden.
    BoundedBuffer<int, 5> buffer;
    std::atomic<int> consumed{0};
    std::thread producer([&] {
        for (int i = 1; i <= 100; ++i) buffer.push(i);
        buffer.push(-1);
    });
    std::thread consumer([&] {
        int sum = 0;
        while (true) {
            const int value = buffer.pop();
            if (value == -1) break;
            sum += value;
            ++consumed;
        }
        require(sum == 5050, "el buffer debe conservar la suma y el orden de los datos");
    });
    producer.join();
    consumer.join();
    require(consumed == 100, "el consumidor debe recibir los 100 elementos");

    // Prueba 2: una sola pista nunca se entrega a dos solicitudes a la vez.
    RunwayManager runway(1, 50ms);
    std::atomic<int> completed{0};
    std::vector<std::thread> flights;
    for (int i = 0; i < 5; ++i) {
        flights.emplace_back([&, i] {
            const auto result = runway.useRunway("TEST" + std::to_string(i), i + 1, 15ms);
            require(result.runwayId == 1, "la unica pista debe ser la numero 1");
            ++completed;
        });
    }
    for (auto& t : flights) t.join();
    require(completed == 5, "todos los vuelos deben completar su uso de pista");
    require(runway.waitingCount() == 0, "la cola de espera debe quedar vacia");

    // Prueba 3: eviction, swap y recuperacion del contenido de una pagina.
    const std::string swapPath = "self_test_swap.bin";
    MemoryManager memory(64, 2, ReplacementPolicy::FIFO, swapPath);
    memory.writeByte(0, 0x5A);
    for (int page = 1; page <= 8; ++page) {
        memory.writeByte(page * 64, static_cast<std::uint8_t>(page));
    }
    const auto restored = memory.readByte(0);
    require(restored.first == 0x5A, "el contenido debe recuperarse desde swap sin alteraciones");
    require(restored.second.physicalAddress >= 0, "la direccion fisica debe ser no negativa");
    require(restored.second.physicalAddress < 128, "la direccion fisica debe quedar dentro de los dos marcos");
    const auto stats = memory.metrics();
    require(stats.pageFaults > 0, "la saturacion debe generar fallos de pagina");
    require(stats.swapWrites > 0, "los reemplazos deben escribir paginas en swap");

    // Prueba 4: los tres algoritmos se ejecutan sobre la misma cadena de referencias.
    const std::vector<int> refs{7, 0, 1, 2, 0, 3, 0, 4, 2, 3, 0, 3, 2};
    for (const auto policy : {ReplacementPolicy::FIFO, ReplacementPolicy::LRU,
                              ReplacementPolicy::CLOCK}) {
        const auto result = MemoryManager::benchmark(refs, 3, policy);
        require(result.metrics.accesses == refs.size(), "cada referencia debe contarse como un acceso");
        require(result.metrics.pageFaults + result.metrics.hits == refs.size(), "fallos y aciertos deben sumar los accesos");
    }

    std::cout << "Todas las pruebas de Entrega finalizaron correctamente.\n";
    return 0;
}
