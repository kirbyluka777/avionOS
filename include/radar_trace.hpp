#pragma once

#include <iomanip>
#include <sstream>
#include <string>

struct RadarTrace {
    std::string flightCode;
    double latitude{};
    double longitude{};
    int altitudeFeet{};
    int speedKnots{};
    bool stop{false};

    static RadarTrace shutdownSignal() {
        RadarTrace signal;
        signal.stop = true;
        return signal;
    }

    std::string serialize() const {
        std::ostringstream out;
        out << flightCode << '|' << std::fixed << std::setprecision(5)
            << latitude << '|' << longitude << '|'
            << altitudeFeet << '|' << speedKnots;
        return out.str();
    }
};
