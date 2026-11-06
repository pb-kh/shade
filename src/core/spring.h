#pragma once

#include <QtGlobal>

#include <algorithm>
#include <cmath>

namespace shade::core
{

// A damped harmonic spring. Every animated quantity in the shade is a spring:
// retargeting mid-flight keeps position and velocity, so animations are
// always interruptible and never jump. An under-damped spring (ratio < 1)
// overshoots a little, which is the shade's "bounce".
struct Spring
{
    qreal value = 0.0;
    qreal velocity = 0.0;
    qreal target = 0.0;
    qreal stiffness = 420.0; // 1/s^2
    qreal damping = 0.7;     // damping ratio (1 = critical)
    qreal epsilon = 0.05;    // settle threshold (units of value)

    void step(qreal dt)
    {
        if (dt <= 0.0) return;
        const qreal c = 2.0 * damping * std::sqrt(stiffness);
        // Fixed small sub-steps keep the integration stable for any frame
        // interval (semi-implicit Euler).
        const int n = std::max(1, static_cast<int>(std::ceil(dt / (1.0 / 480.0))));
        const qreal h = dt / n;
        for (int i = 0; i < n; ++i) {
            const qreal a = -stiffness * (value - target) - c * velocity;
            velocity += a * h;
            value += velocity * h;
        }
        if (settled()) snap();
    }

    bool settled() const
    {
        return std::abs(value - target) < epsilon && std::abs(velocity) < epsilon * 20.0;
    }

    void snap()
    {
        value = target;
        velocity = 0.0;
    }

    void reset(qreal v)
    {
        value = target = v;
        velocity = 0.0;
    }
};

} // namespace shade::core
