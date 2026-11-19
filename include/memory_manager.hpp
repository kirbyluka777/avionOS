#pragma once

#include "radar_trace.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

enum class ReplacementPolicy { FIFO, LRU, CLOCK };

inline std::string policyName(ReplacementPolicy policy) {
    switch (policy) {
        case ReplacementPolicy::FIFO: return "FIFO";
        case ReplacementPolicy::LRU: return "LRU";
        case ReplacementPolicy::CLOCK: return "Reloj";
    }
    return "Desconocido";
}

struct MemoryMetrics {
    std::size_t accesses{0};
    std::size_t hits{0};
    std::size_t pageFaults{0};
    std::size_t replacements{0};
    std::size_t swapWrites{0};

    double hitRatio() const {
        return accesses == 0 ? 0.0 : static_cast<double>(hits) / static_cast<double>(accesses);
    }
};

struct BenchmarkResult {
    ReplacementPolicy policy{};
    std::size_t frames{};
    MemoryMetrics metrics;
};

struct TranslationResult {
    int virtualAddress{};
    int physicalAddress{};
    int pageId{};
    int frameId{};
    int offset{};
    bool pageFault{false};
};

/**
 * @class MemoryManager
 * @brief Administra la memoria virtual del subsistema de radar.
 *
 * Simula el funcionamiento de un administrador de memoria virtual
 * encargado de almacenar las trazas de posición de las aeronaves.
 * 
 * - Tabla de páginas para relacionar páginas virtuales con marcos.
 * 
 * - Traducción de direcciones virtuales a físicas.
 * 
 * - Detección y tratamiento de fallos de página.
 * 
 * - Reemplazo de páginas mediante FIFO, LRU y CLOCK.
 * 
 * - Almacenamiento secundario simulado mediante un archivo Swap.
 * 
 * - Recuperación de páginas previamente desalojadas.
 * 
 * - Registro de métricas de rendimiento.
 *
 * Utiliza `std::mutex` para proteger las estructuras compartidas
 * durante las operaciones de lectura, escritura y traducción.
 *
 * @note Cada página tiene un tamaño fijo establecido en el constructor.
 * @note El archivo Swap representa el almacenamiento secundario simulado.
 */
class MemoryManager {
public:
    MemoryManager(std::size_t pageSize,
                  std::size_t physicalFrames,
                  ReplacementPolicy policy,
                  std::string swapPath = "swap.bin")
        : pageSize_(pageSize), frames_(physicalFrames), policy_(policy), swapPath_(std::move(swapPath)) {
        if (pageSize_ == 0 || physicalFrames == 0) {
            throw std::invalid_argument("El tamaño de página y los marcos deben ser mayores que cero");
        }
        openSwapFile();
    }

    MemoryManager(const MemoryManager&) = delete;
    MemoryManager& operator=(const MemoryManager&) = delete;

    TranslationResult translateAddress(int virtualAddress) {
        std::lock_guard<std::mutex> lock(mutex_);
        return translateLocked(virtualAddress);
    }

    TranslationResult writeByte(int virtualAddress, std::uint8_t value) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto result = translateLocked(virtualAddress);
        auto& frame = frames_.at(static_cast<std::size_t>(result.frameId));
        frame.bytes.at(static_cast<std::size_t>(result.offset)) = value;
        frame.dirty = true;
        return result;
    }

    std::pair<std::uint8_t, TranslationResult> readByte(int virtualAddress) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto result = translateLocked(virtualAddress);
        const auto value = frames_.at(static_cast<std::size_t>(result.frameId))
                               .bytes.at(static_cast<std::size_t>(result.offset));
        return {value, result};
    }

    int storeRadarTrace(const RadarTrace& trace) {
        std::lock_guard<std::mutex> lock(mutex_);
        const int pageId = nextPageId_++;
        const int virtualAddress = checkedVirtualAddress(pageId, 0);
        const int frameId = ensurePageLoadedLocked(pageId);
        auto& frame = frames_.at(static_cast<std::size_t>(frameId));
        std::fill(frame.bytes.begin(), frame.bytes.end(), 0);
        const std::string serialized = trace.serialize();
        const std::size_t bytesToCopy = std::min(pageSize_, serialized.size());
        std::copy_n(serialized.begin(), bytesToCopy, frame.bytes.begin());
        frame.dirty = true;
        frame.referenceBit = true;
        frame.lastUsed = ++logicalClock_;
        ++metrics_.accesses;
        return virtualAddress;
    }

    MemoryMetrics metrics() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return metrics_;
    }

    std::size_t residentPages() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<std::size_t>(std::count_if(frames_.begin(), frames_.end(),
                                                      [](const Frame& frame) { return frame.pageId.has_value(); }));
    }

    std::size_t pageSize() const noexcept { return pageSize_; }
    std::size_t frameCount() const noexcept { return frames_.size(); }
    ReplacementPolicy policy() const noexcept { return policy_; }

    static BenchmarkResult benchmark(const std::vector<int>& pageReferences,
                                     std::size_t frameCount,
                                     ReplacementPolicy policy) {
        if (frameCount == 0) throw std::invalid_argument("El benchmark requiere al menos un marco");
        struct SimFrame {
            int page{-1};
            std::size_t loadedAt{0};
            std::size_t lastUsed{0};
            bool referenceBit{false};
        };
        std::vector<SimFrame> frames(frameCount);
        MemoryMetrics metrics;
        std::size_t logicalTime = 0;
        std::size_t clockHand = 0;

        for (int page : pageReferences) {
            ++metrics.accesses;
            ++logicalTime;
            auto hit = std::find_if(frames.begin(), frames.end(),
                                    [page](const SimFrame& frame) { return frame.page == page; });
            if (hit != frames.end()) {
                ++metrics.hits;
                hit->lastUsed = logicalTime;
                hit->referenceBit = true;
                continue;
            }

            ++metrics.pageFaults;
            auto empty = std::find_if(frames.begin(), frames.end(),
                                      [](const SimFrame& frame) { return frame.page == -1; });
            std::size_t victimIndex;
            if (empty != frames.end()) {
                victimIndex = static_cast<std::size_t>(std::distance(frames.begin(), empty));
            } else {
                ++metrics.replacements;
                if (policy == ReplacementPolicy::FIFO) {
                    victimIndex = static_cast<std::size_t>(std::distance(
                        frames.begin(), std::min_element(frames.begin(), frames.end(),
                            [](const SimFrame& a, const SimFrame& b) { return a.loadedAt < b.loadedAt; })));
                } else if (policy == ReplacementPolicy::LRU) {
                    victimIndex = static_cast<std::size_t>(std::distance(
                        frames.begin(), std::min_element(frames.begin(), frames.end(),
                            [](const SimFrame& a, const SimFrame& b) { return a.lastUsed < b.lastUsed; })));
                } else {
                    while (frames[clockHand].referenceBit) {
                        frames[clockHand].referenceBit = false;
                        clockHand = (clockHand + 1) % frameCount;
                    }
                    victimIndex = clockHand;
                    clockHand = (clockHand + 1) % frameCount;
                }
            }

            frames[victimIndex] = SimFrame{page, logicalTime, logicalTime, true};
            if (policy == ReplacementPolicy::CLOCK) {
            }
        }
        return BenchmarkResult{policy, frameCount, metrics};
    }

private:
    struct PageEntry {
        bool present{false};
        int frame{-1};
        bool inSwap{false};
    };

    struct Frame {
        std::optional<int> pageId;
        std::vector<std::uint8_t> bytes;
        std::size_t loadedAt{0};
        std::size_t lastUsed{0};
        bool referenceBit{false};
        bool dirty{false};

        explicit Frame(std::size_t pageSize = 0) : bytes(pageSize, 0) {}
    };

    void openSwapFile() {
        swapFile_.open(swapPath_, std::ios::in | std::ios::out | std::ios::binary);
        if (!swapFile_.is_open()) {
            std::ofstream create(swapPath_, std::ios::out | std::ios::binary);
            create.close();
            swapFile_.clear();
            swapFile_.open(swapPath_, std::ios::in | std::ios::out | std::ios::binary);
        }
        if (!swapFile_.is_open()) {
            throw std::runtime_error("No se pudo crear o abrir el archivo de intercambio: " + swapPath_);
        }
        for (auto& frame : frames_) {
            frame.bytes.assign(pageSize_, 0);
        }
    }

    int checkedVirtualAddress(int pageId, int offset) const {
        const auto address = static_cast<long long>(pageId) * static_cast<long long>(pageSize_) + offset;
        if (address > std::numeric_limits<int>::max()) {
            throw std::overflow_error("La dirección virtual excede el rango entero soportado");
        }
        return static_cast<int>(address);
    }

    TranslationResult translateLocked(int virtualAddress) {
        if (virtualAddress < 0) throw std::invalid_argument("La dirección virtual no puede ser negativa");
        const int pageId = static_cast<int>(static_cast<std::size_t>(virtualAddress) / pageSize_);
        const int offset = static_cast<int>(static_cast<std::size_t>(virtualAddress) % pageSize_);
        const bool wasResident = pageTable_.contains(pageId) && pageTable_.at(pageId).present;
        const int frameId = ensurePageLoadedLocked(pageId);
        ++metrics_.accesses;
        auto& frame = frames_.at(static_cast<std::size_t>(frameId));
        frame.lastUsed = ++logicalClock_;
        frame.referenceBit = true;
        const int physicalAddress = static_cast<int>(static_cast<std::size_t>(frameId) * pageSize_ +
                                                     static_cast<std::size_t>(offset));
        return TranslationResult{virtualAddress, physicalAddress, pageId, frameId, offset, !wasResident};
    }

    int ensurePageLoadedLocked(int pageId) {
        auto entryIt = pageTable_.find(pageId);
        if (entryIt != pageTable_.end() && entryIt->second.present) {
            ++metrics_.hits;
            return entryIt->second.frame;
        }

        ++metrics_.pageFaults;
        auto empty = std::find_if(frames_.begin(), frames_.end(),
                                  [](const Frame& frame) { return !frame.pageId.has_value(); });
        std::size_t frameIndex;
        if (empty != frames_.end()) {
            frameIndex = static_cast<std::size_t>(std::distance(frames_.begin(), empty));
        } else {
            ++metrics_.replacements;
            frameIndex = selectVictimLocked();
            Frame& victim = frames_.at(frameIndex);
            if (victim.pageId.has_value()) {
                writeSwapLocked(*victim.pageId, victim.bytes);
                auto& victimEntry = pageTable_[*victim.pageId];
                victimEntry.present = false;
                victimEntry.frame = -1;
                victimEntry.inSwap = true;
                ++metrics_.swapWrites;
            }
        }

        std::vector<std::uint8_t> loadedData(pageSize_, 0);
        const auto oldEntry = pageTable_.find(pageId);
        if (oldEntry != pageTable_.end() && oldEntry->second.inSwap) {
            readSwapLocked(pageId, loadedData);
        }

        Frame& destination = frames_.at(frameIndex);
        destination.pageId = pageId;
        destination.bytes = std::move(loadedData);
        destination.loadedAt = ++logicalClock_;
        destination.lastUsed = logicalClock_;
        destination.referenceBit = true;
        destination.dirty = false;
        PageEntry& newEntry = pageTable_[pageId];
        newEntry.present = true;
        newEntry.frame = static_cast<int>(frameIndex);
        return static_cast<int>(frameIndex);
    }

    std::size_t selectVictimLocked() {
        if (policy_ == ReplacementPolicy::FIFO) {
            const auto victim = std::min_element(frames_.begin(), frames_.end(),
                [](const Frame& a, const Frame& b) { return a.loadedAt < b.loadedAt; });
            return static_cast<std::size_t>(std::distance(frames_.begin(), victim));
        }
        if (policy_ == ReplacementPolicy::LRU) {
            const auto victim = std::min_element(frames_.begin(), frames_.end(),
                [](const Frame& a, const Frame& b) { return a.lastUsed < b.lastUsed; });
            return static_cast<std::size_t>(std::distance(frames_.begin(), victim));
        }

        while (frames_.at(clockHand_).referenceBit) {
            frames_.at(clockHand_).referenceBit = false;
            clockHand_ = (clockHand_ + 1) % frames_.size();
        }
        const std::size_t victim = clockHand_;
        clockHand_ = (clockHand_ + 1) % frames_.size();
        return victim;
    }

    void writeSwapLocked(int pageId, const std::vector<std::uint8_t>& bytes) {
        const auto offset = static_cast<std::streamoff>(pageId) * static_cast<std::streamoff>(pageSize_);
        swapFile_.clear();
        swapFile_.seekp(offset, std::ios::beg);
        swapFile_.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        swapFile_.flush();
        if (!swapFile_) throw std::runtime_error("Error al escribir la página en el área swap");
    }

    void readSwapLocked(int pageId, std::vector<std::uint8_t>& bytes) {
        const auto offset = static_cast<std::streamoff>(pageId) * static_cast<std::streamoff>(pageSize_);
        swapFile_.clear();
        swapFile_.seekg(offset, std::ios::beg);
        swapFile_.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (swapFile_.gcount() != static_cast<std::streamsize>(bytes.size())) {
            throw std::runtime_error("Página incompleta o corrupta en el área swap");
        }
    }

    std::size_t pageSize_;
    std::vector<Frame> frames_;
    ReplacementPolicy policy_;
    std::string swapPath_;
    std::fstream swapFile_;
    std::unordered_map<int, PageEntry> pageTable_;
    std::size_t nextPageId_{0};
    std::size_t logicalClock_{0};
    std::size_t clockHand_{0};
    MemoryMetrics metrics_;
    mutable std::mutex mutex_;
};
