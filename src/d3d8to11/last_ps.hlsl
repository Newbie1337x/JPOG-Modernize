cbuffer PSConstants : register(b0) {
  float4 c[8];
  float4 textureFactor;
  float4 alphaRef;
  float4 bumpMat[8];
  float4 bumpLum[8];
  float4 fogColor;
  float4 fogParams;
};
Texture2D tex0 : register(t0);
SamplerState smp0 : register(s0);
Texture2D tex1 : register(t1);
SamplerState smp1 : register(s1);
Texture2D tex2 : register(t2);
SamplerState smp2 : register(s2);
struct VSOut {
  float4 pos : SV_Position;
  float4 d0 : COLOR0;
  float4 d1 : COLOR1;
  float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;
  float4 t4 : TEXCOORD4; float4 t5 : TEXCOORD5; float4 t6 : TEXCOORD6; float4 t7 : TEXCOORD7;
  float fog : FOG;
  float3 world : TEXCOORD8; float3 normal : TEXCOORD9;
};
cbuffer Sun : register(b1) { float4 sunRead[4]; float4 sunWrite[4]; float4 clipToWorld[4]; float4 sunDirection; float4 sunSettings; float4 sunFlags; float4 sunControl; float4 sunCamera; float4 gameAmbient; float4 gameSun; float4 relightParams; float4 terrainMap; float4 todSun; float4 todAmbient; float4 todNight; float4 rimColor; float4 fogTint; float4 grassGrade; float4 specParams; float4 gameSunDir; };
float3 ToLinear(float3 c) { return pow(max(c, 0), 2.2); }
float3 Shoulder(float3 c) {
  const float start = 0.8; float peak = max(c.r, max(c.g, c.b));
  if (peak <= start) return c;
  const float d = 1 - start; float np = 1 - d * d / (peak + d - start);
  c *= np / peak;
  float g = 1 - 1 / (0.15 * (peak - np) + 1);
  return lerp(c, np.xxx, g); }
float3 LinearToOutput(float3 c) {
  c *= relightParams.w;
  if (gameAmbient.w > 0.5) c = Shoulder(c);
  return pow(saturate(c), 1 / 2.2); }
float3 GameAmbient(float3 N) {
  float up = saturate(-N.y * 0.5 + 0.5);
  float3 tint = lerp(float3(0.95, 0.85, 0.65), float3(0.92, 1.02, 1.25), up);
  return ToLinear(saturate(gameAmbient.rgb)) * relightParams.y * lerp(1, tint, relightParams.z) * todAmbient.rgb; }
float3 GameSun() { return max(ToLinear(saturate(gameAmbient.rgb + gameSun.rgb)) - ToLinear(saturate(gameAmbient.rgb)), 0) * relightParams.x * todSun.rgb; }
Texture2D<float> terrainHeight : register(t15); SamplerState terrainSmp : register(s11);
float TerrainH(float2 p) { return terrainHeight.SampleLevel(terrainSmp, (p + 0.5) * terrainMap.yz, 0); }
bool TerrainInside(float2 p) { float2 uv = (p + 0.5) * terrainMap.yz; return all(uv > 0) && all(uv < 1); }
float3 TerrainNormal(float2 p) {
  float hx = TerrainH(p + float2(1, 0)) - TerrainH(p - float2(1, 0));
  float hz = TerrainH(p + float2(0, 1)) - TerrainH(p - float2(0, 1));
  return normalize(float3(-0.5 * hx, -1, -0.5 * hz)); }
float HorizonShadow(float3 w, float3 L) {
  float len = length(L.xz); if (len < 1e-3) return 1;
  float2 d = L.xz / len; float rise = -L.y / len;
  if (rise <= 0) return 0;
  float h0 = -w.y + 0.25, vis = 1, t = 1.5;
  [loop] for (int k = 0; k < 24; k++) {
    float2 q = w.xz + d * t; if (!TerrainInside(q)) break;
    vis = min(vis, 1 - saturate((TerrainH(q) - (h0 + t * rise)) / (0.4 + 0.03 * t)));
    t *= 1.27;
  }
  return vis; }
float3 RimLight(float3 N, float3 world) {
  if (todAmbient.w <= 0) return 0;
  float3 V = sunCamera.xyz - world; float vl = dot(V, V); if (vl < 1e-8) return 0;
  V *= rsqrt(vl); if (dot(N, V) < 0) N = -N;
  float3 L = normalize(sunDirection.xyz);
  float nv = saturate(dot(N, V)), nl = dot(N, L);
  float edge = pow(1 - nv, 3);
  float facing = saturate(nl * 0.7 + 0.3), back = saturate(dot(-V, L) * 0.5 + 0.5);
  float soft = edge * facing * lerp(0.5, 1.5, back);
  float thin = smoothstep(1 - specParams.w, 1, 1 - nv) * saturate(nl * 2.5) * lerp(0.4, 2, back);
  return rimColor.rgb * lerp(soft, thin, specParams.z) * todAmbient.w; }
float4 RelightSurface(float4 color, VSOut i) {
  if (sunFlags.x < 0.5 || sunFlags.y > 0.5 || sunControl.x < 0.5) return color;
  bool grass = sunFlags.w == 4 && sunFlags.z > 0.5;
  bool rigid = sunFlags.w == 3 && sunFlags.z > 0.5;
  if (sunFlags.w != 1 && sunFlags.w != 2 && !rigid && !grass) return color;
  if (sunControl.y == 1) return color;
  float3 rawN = cross(ddy(i.world), ddx(i.world));
  if (sunFlags.w == 1 && dot(i.normal,i.normal) > 1e-8) rawN = i.normal;
  float nlen = dot(rawN, rawN); if (nlen < 1e-12) return color;
  float3 N = rawN * rsqrt(nlen);
  if (sunFlags.w != 1 && N.y > 0) N = -N;
  bool field = sunFlags.w != 1 && terrainMap.x > 0.5 && TerrainInside(i.world.xz);
  if (field) N = TerrainNormal(i.world.xz);
  else if (grass) N = float3(0, -1, 0);
  if (sunControl.y == 2) return float4(N*0.5+0.5,color.a);
  float fieldNdl = 1, fieldShadow = 1;
  if (field) {
    float3 L = normalize(sunDirection.xyz);
    fieldNdl = saturate(dot(N, L));
    fieldShadow = lerp(1, HorizonShadow(i.world, L), terrainMap.w);
  }
  if (rigid) {
    const float3 luma = float3(0.299, 0.587, 0.114);
    float3 A = saturate(gameAmbient.rgb), S = gameSun.rgb, baked = saturate(i.d0.rgb);
    float f = saturate((dot(baked, luma) - dot(A, luma)) / max(dot(S, luma), 1e-3));
    float3 tint = clamp(baked / max(saturate(A + S * f), 1e-3), 0, 2);
    float sunF = f;
    if (field) sunF = fieldNdl * min(fieldShadow, lerp(1, fieldNdl > 0.05 ? saturate(f / fieldNdl) : 1, sunControl.w));
    float3 light = GameAmbient(N) + GameSun() * sunF;
    return float4(LinearToOutput(ToLinear(color.rgb * tint) * light), color.a);
  }
  if (grass) {
    float gameNdl = field ? saturate(dot(N, normalize(gameSunDir.xyz))) : 1;
    float3 base = ToLinear(saturate(gameAmbient.rgb + gameSun.rgb * gameNdl));
    float sunF = field ? fieldNdl * fieldShadow : 1;
    float3 light = GameAmbient(N) + GameSun() * sunF;
    light += GameSun() * saturate(i.t7.x * 1.5) * rimColor.w;
    return float4(LinearToOutput(ToLinear(color.rgb) * light / max(base, 0.05)), color.a);
  }
  float ndl = saturate(dot(N, sunDirection.xyz));
  if (sunFlags.w == 2) {
    float baked = saturate(i.t7.x);
    ndl = field ? fieldNdl * min(fieldShadow, lerp(1, fieldNdl > 0.05 ? saturate(baked / fieldNdl) : 1, sunControl.w)) : baked;
  }
  float3 light = GameAmbient(N) + GameSun() * ndl;
  float3 rim = sunFlags.w == 1 ? RimLight(N, i.world) : 0;
  light += rim;
  float3 sheen = 0;
  if (sunFlags.w == 1 && todSun.w > 0) {
    float3 V = normalize(sunCamera.xyz - i.world), Ns = dot(N, V) < 0 ? -N : N;
    float3 L = normalize(sunDirection.xyz), H = normalize(V + L);
    float nl = saturate(dot(Ns, L)), nv = max(saturate(dot(Ns, V)), 1e-3), nh = saturate(dot(Ns, H)), vh = saturate(dot(V, H));
    float a = specParams.x * specParams.x, a2 = a * a, dd = nh * nh * (a2 - 1) + 1;
    float D = a2 / (3.14159265 * max(dd * dd, 1e-6));
    float k = (specParams.x + 1) * (specParams.x + 1) / 8;
    float G = nv / (nv * (1 - k) + k) * nl / max(nl * (1 - k) + k, 1e-3);
    float F = specParams.y + (1 - specParams.y) * pow(1 - vh, 5);
    sheen = GameSun() * (D * G * F / max(4 * nv, 1e-3)) * 3.14159265 * todSun.w;
  }
  if (sunFlags.w == 2) light = lerp(light, 1, saturate(i.d1.w));
  return float4(LinearToOutput(ToLinear(color.rgb) * light + rim * grassGrade.z + sheen), color.a);
}
float4 ApplySun(float4 color, VSOut i) {
  if (sunControl.y == 13 && sunFlags.y < 0.5) {
    float k = sunFlags.x > 0.5 ? sunFlags.w : -1;
    float3 c = k == 1 ? float3(1, 0.2, 0.2) : k == 2 ? float3(0.2, 1, 0.2) : k == 3 ? float3(0.2, 0.4, 1) : k == 4 ? float3(1, 1, 0.2) : k == 0 ? float3(1, 0.3, 1) : float3(0.5, 0.5, 0.5);
    return float4(lerp(color.rgb, c, 0.7), color.a); }
  bool relit = sunFlags.x > 0.5 && sunFlags.y < 0.5 && sunControl.x > 0.5 &&
    (sunFlags.w == 1 || sunFlags.w == 2 || ((sunFlags.w == 3 || sunFlags.w == 4) && sunFlags.z > 0.5));
  float4 lit = RelightSurface(color, i);
  if (!relit && todNight.w > 0.5) {
    lit.rgb *= todNight.rgb;
    if (sunFlags.w == 4) lit.rgb += color.rgb * rimColor.rgb * saturate(i.t7.x * 1.5) * rimColor.w;
  }
  if (sunFlags.w == 4 && sunFlags.y < 0.5 && grassGrade.w > 0.5)
    lit.rgb = lerp(dot(lit.rgb, float3(0.2126, 0.7152, 0.0722)).xxx, lit.rgb, grassGrade.x) * grassGrade.y;
  return lit; }
Texture2D normalTex : register(t9);
float4 ApplyNormalMap(float4 color, VSOut i) {
  if (sunFlags.x < 0.5 || sunFlags.y > 0.5) return color;
  float3 n = normalTex.Sample(smp0, i.t0.xy).xyz * 2 - 1;
  if (sunCamera.w < 0) n.y = -n.y;
  n.z = sqrt(saturate(1 - dot(n.xy, n.xy)));
  float3 dp1 = ddx(i.world), dp2 = ddy(i.world);
  float2 duv1 = ddx(i.t0.xy), duv2 = ddy(i.t0.xy);
  float3 N0 = cross(dp1, dp2); float gl = dot(N0, N0);
  if (gl < 1e-20) return color;
  N0 *= rsqrt(gl);
  float3 Ng = dot(N0, sunCamera.xyz - i.world) < 0 ? -N0 : N0;
  float3 dp2perp = cross(dp2, N0), dp1perp = cross(N0, dp1);
  float3 T = dp2perp * duv1.x + dp1perp * duv2.x, B = dp2perp * duv1.y + dp1perp * duv2.y;
  float tl = max(dot(T, T), dot(B, B));
  if (tl < 1e-20) return color;
  float inv = rsqrt(tl);
  float3 Nw = normalize(T * inv * n.x + B * inv * n.y + Ng * n.z);
  if (sunControl.y == 11) return float4(Nw * 0.5 + 0.5, 1);
  float k = abs(sunCamera.w);
  float relief = 1 + k * (saturate(dot(Nw, sunDirection.xyz)) - saturate(dot(Ng, sunDirection.xyz)));
  relief *= lerp(1, n.z, k * 0.35);
  return float4(color.rgb * clamp(relief, 0.35, 1.7), color.a); }
cbuffer Material:register(b3){float4 materialSurface;float4 materialOptics;};
float3 MaterialNormal(VSOut i){
 float3 raw=cross(ddy(i.world),ddx(i.world));
 if(dot(i.normal,i.normal)>1e-8)raw=i.normal;
 if(dot(raw,raw)<1e-12)return float3(0,-1,0);
 float3 N=normalize(raw);if(dot(N,sunCamera.xyz-i.world)<0)N=-N;
 float3 n=normalTex.Sample(smp0,i.t0.xy).xyz*2-1;
 n.xy*=materialSurface.w;if(sunCamera.w<0)n.y=-n.y;n.z=sqrt(saturate(1-dot(n.xy,n.xy)));
 float3 dp1=ddx(i.world),dp2=ddy(i.world);float2 uv1=ddx(i.t0.xy),uv2=ddy(i.t0.xy);
 float3 a=cross(dp2,N),b=cross(N,dp1);float3 T=a*uv1.x+b*uv2.x,B=a*uv1.y+b*uv2.y;
 float scale=max(dot(T,T),dot(B,B));
 if(scale>1e-12)N=normalize((T*n.x+B*n.y)*rsqrt(scale)+N*n.z);
 return N;}
float4 ApplyMaterial(float4 color,VSOut i){
 if(materialOptics.y>.5){float t=materialOptics.z;
  return float4(t>.5?float3(1,0,0):materialOptics.w<1.5?float3(1,0,1):float3(0,1,1),color.a);}
 float3 N=MaterialNormal(i),eye=sunCamera.xyz-i.world;
 float3 V=dot(eye,eye)>1e-8?normalize(eye):N,L=normalize(sunDirection.xyz);
 float3 halfVector=V+L,H=halfVector/max(length(halfVector),.001);
 float nl=saturate(dot(N,L)),nv=max(.001,saturate(dot(N,V))),nh=saturate(dot(N,H)),vh=saturate(dot(V,H));
 float rough=clamp(materialSurface.x,.1,1),alpha=rough*rough,a2=alpha*alpha;
 float denom=nh*nh*(a2-1)+1,D=a2/(3.14159265*max(denom*denom,.000001));
 float k=(rough+1)*(rough+1)/8;float G=nv/(nv*(1-k)+k)*nl/max(nl*(1-k)+k,.001);
 float3 F0=materialOptics.x.xxx,F=F0+(1-F0)*pow(1-vh,5);
 float3 albedo=pow(max(color.rgb,0),2.2);
 float3 spec=D*G*F/max(4*nv*nl,.001)*materialSurface.y;
 bool game=gameSun.w>.5;
 float3 sunRad=game?GameSun()*3.14159265:sunSettings.y*3.14159265*float3(1,.95,.84);
 float3 direct=((1-F)*albedo/3.14159265+spec)*nl*sunRad;
 float sky=saturate(-N.y*.5+.5);
 float3 ambient=game?albedo*GameAmbient(N):albedo*sunSettings.x*lerp(float3(.38,.34,.27),float3(.78,.88,1),sky);
 float3 R=reflect(-V,N);float horizon=saturate(-R.y*.5+.5);
 float3 environment=lerp(float3(.10,.07,.04),float3(.24,.34,.50),horizon);
 float3 envF=F0+(max((1-rough).xxx,F0)-F0)*pow(1-nv,5);
 float3 indirectSpec=environment*envF*materialSurface.z*(1-.5*rough);
 float3 lit=ambient+direct+indirectSpec;
 if(game){float3 rim=RimLight(N,i.world);lit+=albedo*rim+rim*.08;}
 return float4(game?LinearToOutput(lit):pow(max(lit,0),1/2.2),color.a);}
float4 main(VSOut i) : SV_Target {
  float4 cp[8] = c;
  float4 v0 = i.d0, v1 = i.d1;
  bool relight = sunControl.x > 0.5 && sunFlags.x > 0.5 && sunFlags.y < 0.5;
  if (relight) { v0.rgb = 1; v1.rgb = 0; }
  float4 r0 = 0, r1 = 0, t0 = 0, t1 = 0, t2 = 0, t3 = 0;
  float m0 = 0, m1 = 0;
  t0 = tex0.Sample(smp0, (i.t0).xy);
  t1 = tex1.Sample(smp1, (i.t1).xy);
  if (any(i.t2.xyz < 0.0)) discard;
  float4 q0 = (lerp(t1, t0, cp[1].wwww));
  r1 = q0;
  float4 q1 = ((relight ? r1 : lerp(r1, v1, v1.wwww)));
  r0.xyz = q1.xyz;
  float4 q2 = (t0.wwww * v0.wwww);
  r0.w = q2.w;
  float4 color = saturate(r0);
  color = ApplyMaterial(color,i);
  if (relight && sunControl.y == 0) color.rgb = lerp(color.rgb,i.d1.rgb*fogTint.rgb,saturate(i.d1.w));
  return color;
}
