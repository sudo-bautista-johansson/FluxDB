#pragma once

#include <cmath>
#include <cstdint>

namespace veldradb {
namespace types {

struct Vec2 {
    float x_ = 0.0f;
    float y_ = 0.0f;

    Vec2() = default;
    Vec2(float x, float y) : x_(x), y_(y) {}

    float distance(const Vec2& other) const {
        float dx = x_ - other.x_;
        float dy = y_ - other.y_;
        return std::sqrt(dx * dx + dy * dy);
    }
};

struct Vec3 {
    float x_ = 0.0f;
    float y_ = 0.0f;
    float z_ = 0.0f;

    Vec3() = default;
    Vec3(float x, float y, float z) : x_(x), y_(y), z_(z) {}

    float distance(const Vec3& other) const {
        float dx = x_ - other.x_;
        float dy = y_ - other.y_;
        float dz = z_ - other.z_;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }
};

struct Color {
    uint8_t r = 0, g = 0, b = 0, a = 255;
    
    Color() = default;
    Color(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) : r(r), g(g), b(b), a(a) {}
};

struct AABB {
    Vec3 min;
    Vec3 max;

    AABB() = default;
    AABB(const Vec3& min, const Vec3& max) : min(min), max(max) {}

    bool intersects(const AABB& other) const {
        return (min.x_ <= other.max.x_ && max.x_ >= other.min.x_) &&
               (min.y_ <= other.max.y_ && max.y_ >= other.min.y_) &&
               (min.z_ <= other.max.z_ && max.z_ >= other.min.z_);
    }
    
    bool contains(const Vec3& point) const {
        return (point.x_ >= min.x_ && point.x_ <= max.x_) &&
               (point.y_ >= min.y_ && point.y_ <= max.y_) &&
               (point.z_ >= min.z_ && point.z_ <= max.z_);
    }
};

struct Quaternion {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
    
    Quaternion() = default;
    Quaternion(float x, float y, float z, float w) : x(x), y(y), z(z), w(w) {}
};

} // namespace types
} // namespace veldradb
