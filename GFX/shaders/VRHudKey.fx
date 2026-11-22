// VR desktop window: draws the game's 2D layer (HUD, menus) over the mirrored eye view.
// The layer was drawn on the key colour (VR.bb, VR_KEY_*), which is left out; everything else, black included,
// is drawn opaque.
#include "Common.fx"

float4x4 ViewProj 		: MATRIX_VIEWPROJ;

static const float3 KeyColour = float3(0.0, 0.0, 12.0 / 255.0);
static const float KeyTolerance = 0.035;

sampler ColorMap : register(s0) = sampler_state
{
	MinFilter = Point;
	MagFilter = Point;
	MipFilter = Point;
	AddressU = Clamp;
	AddressV = Clamp;
	AddressW = Clamp;
};

void VS_Key(VS_INPUT input, out float4 Pos : POSITION, out float2 TexCoord : TEXCOORD0)
{
	Pos = mul(input.Pos, ViewProj);
	TexCoord = input.TexCoords;
}

float4 PS_Key(float4 Pos : POSITION, float2 TexCoord : TEXCOORD0) : COLOR
{
	float4 c = tex2D(ColorMap, TexCoord);
	clip(distance(c.rgb, KeyColour) / KeyTolerance - 0.5);
	return float4(c.rgb, 1.0);
}

technique Main
{
	pass p0
	{
		VertexShader = compile vs_3_0 VS_Key();
		PixelShader = compile ps_3_0 PS_Key();
		ZWriteEnable = false;
		ClipPlaneEnable = false;
		Lighting = false;
	}
}
