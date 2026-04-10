// ================================================================
// Minimal matrix math (column-major) + CPU-side Perlin noise
//
// All functions are inline to avoid linker issues.
// ================================================================
#pragma once

#include <cmath>

// ----------------------------------------------------------------
// 4x4 column-major matrix
// ----------------------------------------------------------------

struct Mat4 { float m[16] = {}; };

inline Mat4 mat4_mul(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++) {
            float sum = 0;
            for (int k = 0; k < 4; k++)
                sum += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = sum;
        }
    return r;
}

inline Mat4 mat4_perspective(float fov_y_deg, float aspect, float znear, float zfar) {
    float f = 1.0f / tanf(fov_y_deg * 3.14159265f / 360.0f);
    Mat4 r;
    r.m[0]  = f / aspect;
    r.m[5]  = -f;
    r.m[10] = zfar / (znear - zfar);
    r.m[11] = -1.0f;
    r.m[14] = (znear * zfar) / (znear - zfar);
    return r;
}

inline Mat4 mat4_look_at(float ex, float ey, float ez,
                         float tx, float ty, float tz,
                         float ux, float uy, float uz) {
    float fx = tx-ex, fy = ty-ey, fz = tz-ez;
    float fl = sqrtf(fx*fx+fy*fy+fz*fz);
    fx/=fl; fy/=fl; fz/=fl;
    float rx = fy*uz-fz*uy, ry = fz*ux-fx*uz, rz = fx*uy-fy*ux;
    float rl = sqrtf(rx*rx+ry*ry+rz*rz);
    rx/=rl; ry/=rl; rz/=rl;
    float upx = ry*fz-rz*fy, upy = rz*fx-rx*fz, upz = rx*fy-ry*fx;
    Mat4 m;
    m.m[0]=rx;  m.m[4]=ry;  m.m[8]=rz;   m.m[12]=-(rx*ex+ry*ey+rz*ez);
    m.m[1]=upx; m.m[5]=upy; m.m[9]=upz;  m.m[13]=-(upx*ex+upy*ey+upz*ez);
    m.m[2]=-fx; m.m[6]=-fy; m.m[10]=-fz; m.m[14]=(fx*ex+fy*ey+fz*ez);
    m.m[3]=0;   m.m[7]=0;   m.m[11]=0;   m.m[15]=1;
    return m;
}

inline Mat4 mat4_ortho(float left, float right, float top, float bottom, float znear, float zfar) {
    Mat4 r;
    r.m[0]  = 2.0f / (right - left);
    r.m[5]  = 2.0f / (top - bottom);   // Vulkan Y-down: top < bottom flips
    r.m[10] = 1.0f / (znear - zfar);
    r.m[12] = -(right + left) / (right - left);
    r.m[13] = -(top + bottom) / (top - bottom);
    r.m[14] = znear / (znear - zfar);
    r.m[15] = 1.0f;
    return r;
}

// ----------------------------------------------------------------
// CPU-side Perlin noise for terrain generation
// ----------------------------------------------------------------

inline float hash_dot(float ix, float iy, float fx, float fy) {
    float px = ix * 127.1f + iy * 311.7f;
    float py = ix * 269.5f + iy * 183.3f;
    float gx = -1.0f + 2.0f * (sinf(px) * 43758.5453123f - floorf(sinf(px) * 43758.5453123f));
    float gy = -1.0f + 2.0f * (sinf(py) * 43758.5453123f - floorf(sinf(py) * 43758.5453123f));
    return gx * fx + gy * fy;
}

inline float perlin(float x, float y) {
    float ix = floorf(x), iy = floorf(y);
    float fx = x - ix,    fy = y - iy;
    float ux = fx*fx*(3-2*fx), uy = fy*fy*(3-2*fy);
    float a = hash_dot(ix,iy,fx,fy), b = hash_dot(ix+1,iy,fx-1,fy);
    float c = hash_dot(ix,iy+1,fx,fy-1), d = hash_dot(ix+1,iy+1,fx-1,fy-1);
    return (a+ux*(b-a)) + uy*((c+ux*(d-c)) - (a+ux*(b-a)));
}

inline float fbm(float x, float y, int oct) {
    float v = 0, a = 0.5f;
    for (int i = 0; i < oct; i++) { v += a*perlin(x,y); x*=2; y*=2; a*=0.5f; }
    return v;
}
