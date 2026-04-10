// ================================================================
// Skybox — procedural sky preset generation and UBO fill
// ================================================================

#include "sky.h"
#include "app.h"

#include <cmath>
#include <random>
#include <algorithm>

// ----------------------------------------------------------------
// hsl_to_rgb (declared in client.cpp, will move to vulkan_setup.h)
// ----------------------------------------------------------------
void hsl_to_rgb(float h, float s, float l, float out[3]);

// ----------------------------------------------------------------
// generate_random_preset
// ----------------------------------------------------------------

SkyPreset generate_random_preset(std::mt19937& rng) {
    SkyPreset p{};
    auto rf = [&](float lo=0.f, float hi=1.f) { return std::uniform_real_distribution<float>(lo,hi)(rng); };
    float base_hue=rf(), spread=rf(0.05f,0.55f), sat_base=rf(0.3f,1.0f), bright_base=rf(0.5f,0.95f);
    float drama=rf();
    if (drama<0.12f) { sat_base=rf(0.85f,1.0f); bright_base=rf(0.35f,0.55f); spread=rf(0.35f,0.55f); }
    else if (drama<0.22f) { sat_base=rf(0.15f,0.35f); bright_base=rf(0.65f,0.9f); spread=rf(0.03f,0.12f); }
    else if (drama<0.32f) { sat_base=rf(0.2f,0.55f); bright_base=rf(0.08f,0.2f); }
    else if (drama<0.42f) { base_hue=rf(0.95f,1.08f); sat_base=rf(0.7f,1.0f); bright_base=rf(0.5f,0.8f); spread=rf(0.02f,0.12f); }
    hsl_to_rgb(base_hue, sat_base*rf(0.7f,1.0f), bright_base*rf(0.7f,1.0f), p.col_horizon);
    hsl_to_rgb(fmodf(base_hue+spread*rf(0.1f,0.35f),1.f), sat_base*rf(0.5f,0.9f), bright_base*rf(0.25f,0.55f), p.col_low);
    hsl_to_rgb(fmodf(base_hue+spread*rf(0.4f,0.75f),1.f), sat_base*rf(0.3f,0.7f), bright_base*rf(0.08f,0.28f), p.col_mid);
    hsl_to_rgb(fmodf(base_hue+spread,1.f), sat_base*rf(0.15f,0.5f), bright_base*rf(0.01f,0.08f), p.col_zenith);
    hsl_to_rgb(fmodf(base_hue+rf(-0.06f,0.06f),1.f), rf(0.4f,0.85f), rf(0.75f,1.0f), p.sun_col);
    p.sun_glow_power=rf(32.f,512.f); p.sun_elevation=rf(-0.1f,0.7f); p.haze_intensity=rf(0.05f,0.25f);
    float star_temp=rf();
    hsl_to_rgb(star_temp<0.5f?rf(0.55f,0.7f):rf(0.08f,0.16f), rf(0.1f,0.4f), rf(0.8f,1.0f), p.star_col);
    p.star_density=rf(0.975f,0.995f); p.star_brightness=(bright_base<0.25f)?rf(0.6f,1.4f):rf(0.2f,0.9f);
    p.twinkle_speed=rf(0.5f,3.0f);
    p.nebula_intensity=rf()<0.45f?rf(0.08f,0.35f):0.0f;
    if (bright_base<0.25f) p.nebula_intensity=rf(0.15f,0.4f);
    hsl_to_rgb(rf(),rf(0.5f,1.0f),rf(0.3f,0.7f),p.neb_col1);
    hsl_to_rgb(rf(),rf(0.4f,0.9f),rf(0.2f,0.6f),p.neb_col2);
    hsl_to_rgb(rf(),rf(0.5f,1.0f),rf(0.15f,0.5f),p.neb_col3);
    p.cloud_density=rf()<0.5f?rf(0.3f,0.8f):0.0f; p.cloud_threshold=rf(0.35f,0.55f);
    p.aurora_intensity=rf()<0.25f?rf(0.08f,0.3f):0.0f;
    hsl_to_rgb(rf(0.25f,0.45f),rf(0.7f,1.0f),rf(0.4f,0.85f),p.aurora_col1);
    hsl_to_rgb(rf(0.6f,0.85f),rf(0.6f,0.9f),rf(0.3f,0.65f),p.aurora_col2);
    p.planet_radius=rf()<0.6f?rf(0.02f,0.1f):0.0f;
    float ptheta=rf(0.f,6.28318f), pphi=rf(0.25f,0.75f);
    p.planet_dir[0]=cosf(ptheta)*cosf(pphi); p.planet_dir[1]=sinf(pphi); p.planet_dir[2]=sinf(ptheta)*cosf(pphi);
    hsl_to_rgb(rf(),rf(0.2f,0.6f),rf(0.3f,0.7f),p.planet_col1);
    hsl_to_rgb(rf(),rf(0.2f,0.5f),rf(0.2f,0.6f),p.planet_col2);
    p.seed=rf(0.f,1000.f);
    return p;
}

// ----------------------------------------------------------------
// lerp_preset
// ----------------------------------------------------------------

SkyPreset lerp_preset(const SkyPreset& a, const SkyPreset& b, float t) {
    SkyPreset r;
    const float* ap=reinterpret_cast<const float*>(&a);
    const float* bp=reinterpret_cast<const float*>(&b);
    float* rp=reinterpret_cast<float*>(&r);
    constexpr int N=sizeof(SkyPreset)/sizeof(float);
    for (int i=0;i<N;i++) rp[i]=ap[i]+(bp[i]-ap[i])*t;
    r.seed=b.seed;
    return r;
}

// ----------------------------------------------------------------
// fill_ubo
// ----------------------------------------------------------------

SkyUBOData fill_ubo(const SkyPreset& p, float time) {
    SkyUBOData u{};
    float sun_angle=time*0.015f;
    float sx=cosf(sun_angle), sy=p.sun_elevation+0.1f*sinf(sun_angle*0.3f), sz=sinf(sun_angle);
    float sl=sqrtf(sx*sx+sy*sy+sz*sz);
    u.sun[0]=sx/sl; u.sun[1]=sy/sl; u.sun[2]=sz/sl; u.sun[3]=p.sun_glow_power;
    auto set3w=[](float d[4],const float c[3],float w){d[0]=c[0];d[1]=c[1];d[2]=c[2];d[3]=w;};
    auto set3=[](float d[4],const float c[3]){d[0]=c[0];d[1]=c[1];d[2]=c[2];d[3]=0;};
    set3w(u.horizon,p.col_horizon,p.haze_intensity);
    set3w(u.low,p.col_low,p.star_density); set3w(u.mid,p.col_mid,p.star_brightness);
    set3w(u.zenith,p.col_zenith,p.nebula_intensity); set3(u.sun_color,p.sun_col);
    set3w(u.nebula1,p.neb_col1,p.cloud_density); set3w(u.nebula2,p.neb_col2,p.cloud_threshold);
    set3w(u.nebula3,p.neb_col3,p.aurora_intensity);
    set3w(u.aurora1,p.aurora_col1,p.planet_radius); set3w(u.aurora2,p.aurora_col2,p.seed);
    u.planet_dir[0]=p.planet_dir[0]; u.planet_dir[1]=p.planet_dir[1];
    u.planet_dir[2]=p.planet_dir[2]; u.planet_dir[3]=p.twinkle_speed;
    set3(u.planet_col1,p.planet_col1); set3(u.planet_col2,p.planet_col2);
    set3(u.star_color,p.star_col);
    return u;
}
