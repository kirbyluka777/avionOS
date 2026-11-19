#include "bounded_buffer.hpp"
#include "memory_manager.hpp"
#include "radar_trace.hpp"
#include "runway_manager.hpp"

#include <array>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

int main() {
    std::cout << "============================================================\n"
              << " SISTEMA AUTOMATIZADO - AEROPUERTO MANUEL CARLOS PIAR\n"
              << "============================================================\n\n";

    std::cout << "[1] MOTOR PRODUCTOR-CONSUMIDOR\n";
    BoundedBuffer<RadarTrace, 4> radarQueue;
    MemoryManager memory(128, 3, ReplacementPolicy::LRU, "swap_radar.bin");
    std::mutex outputMutex;

    const std::array<RadarTrace, 8> traces{{
        {"001", 8.2871, -62.7603, 12000, 340, false},
        {"002", 8.2902, -62.7551, 14500, 360, false},
        {"003", 8.3015, -62.7480, 18000, 410, false},
        {"001", 8.3150, -62.7350, 12200, 342, false},
        {"004", 8.3200, -62.7250, 22000, 430, false},
        {"003", 8.3350, -62.7100, 18200, 412, false},
        {"005", 8.3450, -62.7000, 9000, 250, false},
        {"002", 8.3550, -62.6900, 14700, 363, false}
    }};

    std::thread radarProducer([&] {
        for (const auto& trace : traces) {
            radarQueue.push(trace);
            std::this_thread::sleep_for(15ms);
        }
        radarQueue.push(RadarTrace::shutdownSignal());
    });

    std::thread towerConsumer([&] {
        while (true) {
            RadarTrace trace = radarQueue.pop();
            if (trace.stop) break;
            const int virtualAddress = memory.storeRadarTrace(trace);
            const auto translated = memory.translateAddress(virtualAddress);
            std::lock_guard<std::mutex> lock(outputMutex);
            std::cout << "  Radar recibe " << std::left << std::setw(7) << trace.flightCode
                      << " | VA=" << std::setw(4) << translated.virtualAddress
                      << " -> PA=" << std::setw(4) << translated.physicalAddress
                      << " | marco=" << translated.frameId
                      << " | traduccion completada" << '\n';
        }
    });

    radarProducer.join();
    towerConsumer.join();
    const auto memoryStats = memory.metrics();
    std::cout << "  Trazas procesadas: " << traces.size()
              << " | fallos de pagina: " << memoryStats.pageFaults
              << " | escrituras swap: " << memoryStats.swapWrites << "\n\n";

    std::cout << "[2] GESTOR DE PISTAS\n";
    RunwayManager runwayManager(2, 100ms);
    const std::array<std::pair<std::string, int>, 4> requests{{
        {"001", 3}, {"003", 8}, {"002", 2}, {"005", 5}
    }};
    std::vector<std::thread> flightThreads;
    for (const auto& [flight, priority] : requests) {
        flightThreads.emplace_back([&, flight, priority] {
            const auto result = runwayManager.useRunway(flight, priority, 90ms);
            std::lock_guard<std::mutex> lock(outputMutex);
            std::cout << "  Vuelo " << result.flightCode << " utilizo pista "
                      << result.runwayId << " (prioridad "
                      << result.effectivePriority << ")\n";
        });
        std::this_thread::sleep_for(10ms);
    }
    for (auto& thread : flightThreads) thread.join();
    std::cout << "  Solicitudes pendientes al finalizar: " << runwayManager.waitingCount() << "\n\n";

    std::cout << "[3] COMPARACION DE ALGORITMOS DE REEMPLAZO\n";
    const std::vector<int> references{7, 0, 1, 2, 0, 3, 0, 4, 2, 3, 0, 3, 2};

    std::cout << std::left
            << std::setw(15) << "Algoritmo"
            << std::right
            << std::setw(10) << "Fallos"
            << std::setw(12) << "Aciertos"
            << std::setw(14) << "Reemplazos"
            << std::setw(16) << "Tasa aciertos"
            << "\n";
    for (const auto policy : {ReplacementPolicy::FIFO, ReplacementPolicy::LRU,
                              ReplacementPolicy::CLOCK}) {
        const auto result = MemoryManager::benchmark(references, 3, policy);
        std::cout << std::left << std::setw(15) << policyName(policy)
                << std::right
                << std::setw(10) << result.metrics.pageFaults
                << std::setw(12) << result.metrics.hits
                << std::setw(14) << result.metrics.replacements
                << std::setw(15) << std::fixed << std::setprecision(1)
                << (result.metrics.hitRatio() * 100.0) << "%"
                << "\n";
    }

    return 0;
}
