#version 300 es

#define EGL_SCALE_AUTO    0
#define EGL_SCALE_NEAREST 1
#define EGL_SCALE_LINEAR  2
#define EGL_SCALE_LANCZOS 3
#define EGL_SCALE_MAX     4

in  highp vec2 uv;
out highp vec4 color;

uniform sampler2D sampler1;

uniform       int   scaleAlgo;
uniform highp vec2  size;
uniform       int   rotate;

uniform       int   nv;
uniform highp float nvGain;
uniform       int   cbMode;

highp float sinc(highp float x)
{
  return x == 0.0 ? 1.0 : sin(x * 3.141592653589793) * 0.3183098861837907 / x;
}

highp float lanczos(highp vec2 v)
{
  return sinc(v.x) * sinc(v.y) * sinc(v.x) * sinc(v.y);
}

highp vec4 textureLanczos(sampler2D sampler, highp vec2 uv)
{
  uv *= size;
  highp ivec2 c = ivec2(uv);
  highp vec2 d = uv - vec2(c);
  highp float k1 = lanczos(d - vec2(-1, -1));
  highp float k2 = lanczos(d - vec2(-1,  0));
  highp float k3 = lanczos(d - vec2(-1, +1));
  highp float k4 = lanczos(d - vec2( 0, -1));
  highp float k5 = lanczos(d - vec2( 0,  0));
  highp float k6 = lanczos(d - vec2( 0, +1));
  highp float k7 = lanczos(d - vec2(+1, -1));
  highp float k8 = lanczos(d - vec2(+1,  0));
  highp float k9 = lanczos(d - vec2(+1, +1));
  return (
    texelFetch(sampler, c + ivec2(-1, -1), 0) * k1 +
    texelFetch(sampler, c + ivec2(-1,  0), 0) * k2 +
    texelFetch(sampler, c + ivec2(-1, +1), 0) * k3 +
    texelFetch(sampler, c + ivec2( 0, -1), 0) * k4 +
    texelFetch(sampler, c + ivec2( 0,  0), 0) * k5 +
    texelFetch(sampler, c + ivec2( 0, +1), 0) * k6 +
    texelFetch(sampler, c + ivec2(+1, -1), 0) * k7 +
    texelFetch(sampler, c + ivec2(+1,  0), 0) * k8 +
    texelFetch(sampler, c + ivec2(+1, +1), 0) * k9
  ) / (k1 + k2 + k3 + k4 + k5 + k6 + k7 + k8 + k9);
}

void main()
{
  highp vec2 ruv;
  if (rotate == 0) // 0
  {
    ruv = uv;
  }
  else if (rotate == 1) // 90
  {
    ruv.x =  uv.y;
    ruv.y = -uv.x + 1.0f;
  }
  else if (rotate == 2) // 180
  {
    ruv.x = -uv.x + 1.0f;
    ruv.y = -uv.y + 1.0f;
  }
  else if (rotate == 3) // 270
  {
    ruv.x = -uv.y + 1.0f;
    ruv.y =  uv.x;
  }

  switch (scaleAlgo)
  {
    case EGL_SCALE_NEAREST:
      color = texelFetch(sampler1, ivec2(ruv * size), 0);
      break;

    case EGL_SCALE_LINEAR:
      color = texture(sampler1, ruv);
      break;

    case EGL_SCALE_LANCZOS:
      color = textureLanczos(sampler1, ruv);
      break;
  }

  if (cbMode > 0)
  {
    highp float L = (17.8824000 * color.r) + (43.516100 * color.g) + (4.11935 * color.b);
    highp float M = (03.4556500 * color.r) + (27.155400 * color.g) + (3.86714 * color.b);
    highp float S = (00.0299566 * color.r) + (00.184309 * color.g) + (1.46709 * color.b);
    highp float l, m, s;

    if (cbMode == 1) // Protanope
    {
      l = 0.0f * L + 2.02344f * M + -2.52581f * S;
      m = 0.0f * L + 1.0f * M + 0.0f * S;
      s = 0.0f * L + 0.0f * M + 1.0f * S;
    }
    else if (cbMode == 2) // Deuteranope
    {
      l = 1.000000 * L + 0.0f * M + 0.00000 * S;
      m = 0.494207 * L + 0.0f * M + 1.24827 * S;
      s = 0.000000 * L + 0.0f * M + 1.00000 * S;
    }
    else if (cbMode == 3) // Tritanope
    {
      l =  1.000000 * L + 0.000000 * M + 0.0 * S;
      m =  0.000000 * L + 1.000000 * M + 0.0 * S;
      s = -0.395913 * L + 0.801109 * M + 0.0 * S;
    }

    highp vec4 error;
    error.r = ( 0.080944447900 * l) + (-0.13050440900 * m) + ( 0.116721066 * s);
    error.g = (-0.010248533500 * l) + ( 0.05401932660 * m) + (-0.113614708 * s);
    error.b = (-0.000365296938 * l) + (-0.00412161469 * m) + ( 0.693511405 * s);
    error.a = 0.0;

    error = color - error;
    color.g += (error.r * 0.7) + (error.g * 1.0);
    color.b += (error.r * 0.7) + (error.b * 1.0);
  }

  if (nv == 1)
  {
    highp float lumi = 1.0 - (0.2126 * color.r + 0.7152 * color.g + 0.0722 * color.b);
    color *= 1.0 + lumi;
    color *= nvGain;
  }

  color.a = 1.0;
}
