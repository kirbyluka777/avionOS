# Aeropuerto Manuel Carlos Piar

Implementacion en C++ del motor de sincronizacion y del administrador de memoria virtual del radar.

## Requisitos

- Compilador compatible con C++20 (por ejemplo, GCC 10+, Clang 12+ o Visual Studio 2022).
- CMake 3.20 o superior.

## Compilar con CMake

```bash
cmake -S . -B build
cmake --build build
```

## Modulos

- `include/bounded_buffer.hpp`: buffer circular finito Productor-Consumidor, con semaforos contadores y mutex.
- `include/runway_manager.hpp`: asignacion exclusiva de pistas mediante `std::mutex`, `std::unique_lock` y `std::condition_variable`; aplica envejecimiento de prioridades.
- `include/memory_manager.hpp`: MMU simulada, tabla de paginas, marcos, traduccion de direcciones, fallos de pagina, archivo swap binario y politicas FIFO/LRU/Clock.
- `src/main.cpp`: demostracion integrada de los modulos.
- `tests/self_test.cpp`: pruebas automatizadas de buffer, exclusividad, swap y algoritmos.