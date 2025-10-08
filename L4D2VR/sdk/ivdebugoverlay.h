#pragma once

class Vector;
class QAngle;

class IVDebugOverlay
{
public:
    virtual void AddEntityTextOverlay(int ent_index, int line_offset, float duration, int r, int g, int b, int a, const char* format, ...) = 0;
    virtual void AddBoxOverlay(const Vector& origin, const Vector& mins, const Vector& max, const QAngle& orientation, int r, int g, int b, int a, float duration) = 0;
    virtual void AddTriangleOverlay(const Vector& p1, const Vector& p2, const Vector& p3, int r, int g, int b, int a, bool noDepthTest, float duration) = 0;
    virtual void AddLineOverlay(const Vector& origin, const Vector& dest, int r, int g, int b, bool noDepthTest, float duration) = 0;
};
