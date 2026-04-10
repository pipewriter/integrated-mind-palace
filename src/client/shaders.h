#pragma once

#include <cstdint>

// ================================================================
// Shader source strings
// ================================================================

// ----------------------------------------------------------------
// Shader sources — TERRAIN (vertex-colored)
// ----------------------------------------------------------------

static const char* TERRAIN_VERT_SRC = R"(
#version 450
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec3 in_color;

layout(location = 0) out vec3  frag_normal;
layout(location = 1) out float frag_dist;
layout(location = 2) out vec2  frag_uv;

layout(push_constant) uniform PC {
    mat4 mvp;
    vec4 cam_pos;
    float fog_on;
    float highlight;
};

void main() {
    gl_Position = mvp * vec4(in_pos, 1.0);
    frag_normal = in_normal;
    frag_dist   = distance(in_pos, cam_pos.xyz);
    frag_uv     = (in_pos.xz + vec2(511.0)) / vec2(1022.0);
}
)";

static const char* TERRAIN_FRAG_SRC = R"(
#version 450
layout(location = 0) in vec3  frag_normal;
layout(location = 1) in float frag_dist;
layout(location = 2) in vec2  frag_uv;
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform PC {
    mat4 mvp;
    vec4 cam_pos;
    float fog_on;
    float highlight;
};

layout(set = 0, binding = 0) uniform sampler2D terrain_tex;

void main() {
    vec3 N = normalize(frag_normal);

    vec3 tc = texture(terrain_tex, frag_uv).rgb;

    vec3 sun = normalize(vec3(0.8, 0.35, 0.5));
    vec3 color = tc * (max(dot(N, sun), 0.0) * vec3(1,0.95,0.8) + 0.15 * vec3(0.5,0.6,0.8));

    if (fog_on > 0.5) {
        float fog = 1.0 - exp(-frag_dist * 0.004);
        color = mix(color, vec3(0.55, 0.75, 1.0), fog);
    }
    color = color / (1.0 + color);
    color = pow(color, vec3(1.0/2.2));

    out_color = vec4(color, 1.0);
}
)";

// ----------------------------------------------------------------
// Shader sources — TEXTURED QUADS
// ----------------------------------------------------------------

// Vertex shader: transforms position, passes UV to fragment shader
static const char* QUAD_VERT_SRC = R"(
#version 450
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_uv;

layout(location = 0) out vec2 frag_uv;
layout(location = 1) out float frag_dist;

layout(push_constant) uniform PC {
    mat4 mvp;
    vec4 cam_pos;
    float fog_on;
    float highlight;
};

void main() {
    gl_Position = mvp * vec4(in_pos, 1.0);
    frag_uv     = in_uv;
    frag_dist   = distance(in_pos, cam_pos.xyz);
}
)";

// Fragment shader: samples texture via descriptor set binding
static const char* QUAD_FRAG_SRC = R"(
#version 450
layout(location = 0) in vec2 frag_uv;
layout(location = 1) in float frag_dist;
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform PC {
    mat4 mvp;
    vec4 cam_pos;
    float fog_on;
    float highlight;
};

// Texture bound via descriptor set (set=0, binding=0)
layout(set = 0, binding = 0) uniform sampler2D tex;

void main() {
    vec4 color = texture(tex, frag_uv);
    if (color.a < 0.1) discard;   // alpha cutout

    // Selection highlight effects
    if (highlight > 1.5) {
        // Text highlight: cyan recolor
        color.rgb *= vec3(0.2, 1.0, 0.85);
    } else if (highlight > 0.5) {
        // Image/video highlight: golden border
        float edge = min(min(frag_uv.x, 1.0 - frag_uv.x), min(frag_uv.y, 1.0 - frag_uv.y));
        float border = 0.05;
        float t = smoothstep(0.0, border, edge);
        color.rgb = mix(vec3(1.0, 0.75, 0.1), color.rgb, t);
    }

    if (fog_on > 0.5) {
        vec3  sky = vec3(0.55, 0.75, 1.0);
        float fog = 1.0 - exp(-frag_dist * 0.004);
        color.rgb = mix(color.rgb, sky, fog);
    }
    out_color = color;
}
)";

// ----------------------------------------------------------------
// Shader sources — PARAMETRIC SKYBOX (fullscreen triangle, UBO-driven)
// ----------------------------------------------------------------

static const char* SKY_VERT_SRC = R"(
#version 450
layout(location = 0) out vec3 ray_dir;
layout(push_constant) uniform PC {
    vec4 cam_fwd;
    vec4 cam_right;
    vec4 cam_up;
    vec4 params;      // x=aspect, y=tan(fov/2), z=time
};
void main() {
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    vec2 pos = uv * 2.0 - 1.0;
    gl_Position = vec4(pos, 0.9999, 1.0);
    float aspect = params.x;
    float tan_hfov = params.y;
    ray_dir = cam_fwd.xyz
            + pos.x * aspect * tan_hfov * cam_right.xyz
            - pos.y * tan_hfov * cam_up.xyz;
}
)";

static const char* SKY_FRAG_SRC = R"(
#version 450
layout(location = 0) in vec3 ray_dir;
layout(location = 0) out vec4 out_color;
layout(push_constant) uniform PC {
    vec4 cam_fwd; vec4 cam_right; vec4 cam_up; vec4 params;
};
layout(std140, set = 0, binding = 0) uniform SkyParams {
    vec4 u_sun; vec4 u_horizon; vec4 u_low; vec4 u_mid; vec4 u_zenith;
    vec4 u_sun_color; vec4 u_nebula1; vec4 u_nebula2; vec4 u_nebula3;
    vec4 u_aurora1; vec4 u_aurora2; vec4 u_planet_dir;
    vec4 u_planet_col1; vec4 u_planet_col2; vec4 u_star_color; vec4 u_reserved;
};
float hash21(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * vec3(443.897, 441.423, 437.195));
    p3 += dot(p3, p3.yzx + 19.19);
    return fract((p3.x + p3.y) * p3.z);
}
float noise2(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash21(i), b = hash21(i + vec2(1,0));
    float c = hash21(i + vec2(0,1)), d = hash21(i + vec2(1,1));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
float fbm3(vec2 p) { float v=0.0,a=0.5; mat2 rot=mat2(0.8,0.6,-0.6,0.8); for(int i=0;i<3;i++){v+=a*noise2(p);p=rot*p*2.0;a*=0.5;} return v; }
float fbm5(vec2 p) { float v=0.0,a=0.5; mat2 rot=mat2(0.8,0.6,-0.6,0.8); for(int i=0;i<5;i++){v+=a*noise2(p);p=rot*p*2.0;a*=0.5;} return v; }
float fbm6(vec2 p) { float v=0.0,a=0.5; mat2 rot=mat2(0.8,0.6,-0.6,0.8); for(int i=0;i<6;i++){v+=a*noise2(p);p=rot*p*2.0;a*=0.5;} return v; }
void main() {
    vec3 rd = normalize(ray_dir); float time = params.z;
    vec3 sun = normalize(u_sun.xyz); float glow_power = u_sun.w;
    vec3 col_horizon=u_horizon.xyz; float haze_str=u_horizon.w;
    vec3 col_low=u_low.xyz; float star_thresh=u_low.w;
    vec3 col_mid=u_mid.xyz; float star_bright=u_mid.w;
    vec3 col_zenith=u_zenith.xyz; float nebula_int=u_zenith.w;
    vec3 sun_col=u_sun_color.xyz;
    vec3 neb_c1=u_nebula1.xyz; float cloud_dens=u_nebula1.w;
    vec3 neb_c2=u_nebula2.xyz; float cloud_thresh=u_nebula2.w;
    vec3 neb_c3=u_nebula3.xyz; float aurora_int=u_nebula3.w;
    vec3 aurora_c1=u_aurora1.xyz; float planet_rad=u_aurora1.w;
    vec3 aurora_c2=u_aurora2.xyz; float seed=u_aurora2.w;
    vec3 planet_dir=normalize(u_planet_dir.xyz+vec3(0.0001));
    float twinkle_spd=u_planet_dir.w;
    vec3 planet_c1=u_planet_col1.xyz; vec3 planet_c2=u_planet_col2.xyz;
    vec3 star_col=u_star_color.xyz;
    float elev=rd.y; float e=max(elev,0.0);
    vec3 sky = mix(col_horizon, col_low, smoothstep(0.0, 0.12, e));
    sky = mix(sky, col_mid, smoothstep(0.12, 0.35, e));
    sky = mix(sky, col_zenith, smoothstep(0.35, 0.85, e));
    if(elev<0.0){vec3 below_dark=col_horizon*0.12;sky=mix(col_horizon*0.7,below_dark,smoothstep(0.0,-0.4,elev));}
    float sd=dot(rd,sun);
    sky+=sun_col*1.3*smoothstep(0.9996,0.9999,sd);
    sky+=sun_col*pow(max(sd,0.0),glow_power)*0.8;
    sky+=sun_col*0.65*pow(max(sd,0.0),max(glow_power/8.0,4.0))*0.25;
    sky+=sun_col*0.35*pow(max(sd,0.0),4.0)*0.12;
    vec3 sun_tint=sun_col*0.8*pow(max(1.0-sun.y,0.0),2.0);
    sky+=sun_tint*pow(max(sd,0.0),2.0)*0.15;
    if(e>0.05&&star_bright>0.01){vec2 star_uv=rd.xz/max(rd.y,0.001)*200.0;vec2 cell=floor(star_uv);float sv=hash21(cell+seed*100.0);float brightness=step(star_thresh,sv);float twinkle=0.5+0.5*sin(time*twinkle_spd*(2.0+sv*4.0)+sv*100.0);brightness*=twinkle*hash21(cell+99.0+seed*50.0);float star_fade=smoothstep(0.05,0.5,e);float sun_dim=1.0-smoothstep(0.85,0.98,sd);vec3 sc=mix(star_col,star_col*vec3(1.2,0.9,0.7),hash21(cell+50.0));sky+=sc*brightness*star_fade*sun_dim*star_bright;}
    if(e>0.08&&nebula_int>0.01){vec2 neb_uv=rd.xz/max(rd.y,0.01)*0.25;float n1=fbm5(neb_uv+time*0.001+vec2(seed*10.0,42.0));float n2=fbm5(neb_uv*1.7+time*0.0015+vec2(100.0+seed*20.0,seed*5.0));vec3 nebula=mix(neb_c1,neb_c2,n1)+neb_c3*n2*n2;float neb_mask=smoothstep(0.08,0.5,e)*smoothstep(0.3,0.5,n1);sky+=nebula*neb_mask*nebula_int;}
    if(planet_rad>0.005){float pa=acos(clamp(dot(rd,planet_dir),-1.0,1.0));float pr=planet_rad;if(pa<pr*2.0){float body=1.0-smoothstep(pr-0.003,pr,pa);vec2 puv=(rd.xz-planet_dir.xz)/max(pr,0.001)*5.0;float surf=fbm3(puv*3.0+vec2(seed*30.0));vec3 pcol=mix(planet_c1,planet_c2,surf);float plight=dot(planet_dir,sun)*0.4+0.6;float rim=exp(-(pa/pr-0.7)*5.0)*0.4;sky=mix(sky,pcol*plight,body);if(pa<pr*1.5){vec3 rim_col=mix(vec3(0.25,0.45,0.7),sun_col*0.5,0.3);sky+=rim_col*rim*(1.0-body*0.5);}}}
    if(e>0.01&&cloud_dens>0.01){vec2 cuv=rd.xz/max(rd.y,0.01)*2.5;float wind=time*0.008;float c1=fbm6(cuv+vec2(wind,wind*0.3)+seed*7.0);float c2=fbm5(cuv*0.4+vec2(-wind*0.5,wind*0.7)+70.0+seed*11.0);float cloud=smoothstep(cloud_thresh,cloud_thresh+0.2,c1);cloud=max(cloud,smoothstep(cloud_thresh+0.06,cloud_thresh+0.26,c2)*0.5);float cs=pow(max(dot(rd,sun),0.0),2.0);vec3 ccol=mix(col_horizon*0.7,sun_col*vec3(1.0,0.9,0.7),cs);float cfade=smoothstep(0.01,0.12,e)*(1.0-smoothstep(0.5,0.8,e));sky=mix(sky,ccol,cloud*cfade*cloud_dens);}
    if(e>0.25&&aurora_int>0.01){float ax=rd.x*3.0+time*0.01;float ay=e*4.0;float band=sin(ay*6.0+fbm3(vec2(ax,ay)+seed*3.0)*5.0);band=smoothstep(0.5,1.0,band);vec3 acol=mix(aurora_c1,aurora_c2,fbm3(vec2(ax*0.5+time*0.005,ay*0.3)+seed*4.0));float amask=smoothstep(0.25,0.5,e)*(1.0-smoothstep(0.65,0.85,e));sky+=acol*band*amask*aurora_int;}
    if(e>0.1){float cycle=floor(time*0.2+seed);float phase=fract(time*0.2+seed);if(hash21(vec2(cycle,seed*47.0))>0.65){float a1=hash21(vec2(cycle*1.1,seed*51.0))*6.28;float a2=hash21(vec2(cycle*1.2,seed*52.0))*0.4+0.2;vec3 start_dir=normalize(vec3(cos(a1)*cos(a2),sin(a2),sin(a1)*cos(a2)));vec3 travel=normalize(cross(start_dir,vec3(0.0,1.0,0.0)));vec3 streak_pos=normalize(start_dir+travel*phase*0.15);float streak_dot=dot(rd,streak_pos);float streak=smoothstep(0.9998,0.99995,streak_dot);float trail_fade=1.0-phase;sky+=sun_col*streak*trail_fade*3.0*smoothstep(0.1,0.3,e);}}
    float haze=exp(-abs(elev)*6.0); vec3 haze_col=mix(col_horizon,sun_col,pow(max(sd,0.0),3.0));
    sky+=haze_col*haze*haze_str;
    sky=sky/(1.0+sky); sky=pow(sky,vec3(1.0/2.2));
    out_color=vec4(sky,1.0);
}
)";

// ----------------------------------------------------------------
// Shader sources — STRUCTURES (vertex-colored with per-vertex RGB)
// ----------------------------------------------------------------

static const char* STRUCT_VERT_SRC = R"(
#version 450
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec3 in_color;

layout(location = 0) out vec3 frag_color;
layout(location = 1) out vec3 frag_normal;
layout(location = 2) out float frag_dist;

layout(push_constant) uniform PC {
    mat4 mvp;
    vec4 cam_pos;
    float fog_on;
    float highlight;
};

void main() {
    gl_Position = mvp * vec4(in_pos, 1.0);
    frag_color  = in_color;
    frag_normal = in_normal;
    frag_dist   = distance(in_pos, cam_pos.xyz);
}
)";

static const char* STRUCT_FRAG_SRC = R"(
#version 450
layout(location = 0) in vec3  frag_color;
layout(location = 1) in vec3  frag_normal;
layout(location = 2) in float frag_dist;
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform PC {
    mat4 mvp;
    vec4 cam_pos;
    float fog_on;
    float highlight;
};

void main() {
    vec3 N = normalize(frag_normal);
    vec3 sun = normalize(vec3(0.8, 0.35, 0.5));
    vec3 color = frag_color * (max(dot(N, sun), 0.0) * vec3(1,0.95,0.8) + 0.15 * vec3(0.5,0.6,0.8));

    if (fog_on > 0.5) {
        float fog = 1.0 - exp(-frag_dist * 0.004);
        color = mix(color, vec3(0.55, 0.75, 1.0), fog);
    }
    color = color / (1.0 + color);
    color = pow(color, vec3(1.0/2.2));
    out_color = vec4(color, 1.0);
}
)";

// ================================================================
// Sky-related data structures
// ================================================================

struct SkyPushConstants {
    float cam_fwd[4];
    float cam_right[4];
    float cam_up[4];
    float params[4]; // aspect, tan(fov/2), time, pad
};

struct SkyUBOData {
    float sun[4]; float horizon[4]; float low[4]; float mid[4]; float zenith[4];
    float sun_color[4]; float nebula1[4]; float nebula2[4]; float nebula3[4];
    float aurora1[4]; float aurora2[4]; float planet_dir[4];
    float planet_col1[4]; float planet_col2[4]; float star_color[4]; float reserved[4];
};

struct SkyPreset {
    float col_horizon[3], col_low[3], col_mid[3], col_zenith[3];
    float sun_col[3];
    float sun_glow_power, sun_elevation;
    float haze_intensity;
    float star_col[3];
    float star_density, star_brightness, twinkle_speed;
    float nebula_intensity;
    float neb_col1[3], neb_col2[3], neb_col3[3];
    float cloud_density, cloud_threshold;
    float aurora_intensity;
    float aurora_col1[3], aurora_col2[3];
    float planet_radius;
    float planet_dir[3];
    float planet_col1[3], planet_col2[3];
    float seed;
};
