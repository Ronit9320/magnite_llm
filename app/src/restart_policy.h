#pragma once

#include <optional>

// How the desktop app relaunches a backend that died on its own: five attempts with doubling
// delays (1, 2, 4, 8, 16 s), after which the crash is reported. A backend that then stays healthy
// calls noteHealthy(), so a crash hours later starts from the first delay again.
struct RestartPolicy {
    int maxAttempts = 5;
    int baseDelayMs = 1000;

    std::optional<int> nextDelayMs() {
        if (m_attempts >= maxAttempts) return std::nullopt;
        return baseDelayMs << m_attempts++;
    }
    void noteHealthy() { m_attempts = 0; }
    void reset() { m_attempts = 0; }
    int attempts() const { return m_attempts; }

private:
    int m_attempts = 0;
};
