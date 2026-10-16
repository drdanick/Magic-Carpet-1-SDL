// Tier 1: the software rasteriser (DrawTriangle_729A3_72EB3) must not depend on vertex order.
// The terrain renderer splits each map cell along one of two diagonals (by cell parity) and passes
// the same corners in different orders; any order-dependent bias shows up as a checkerboard.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "sub_main.h"
#include "engine/Basic.h"

// Rasteriser state, defined in sub_main.cpp.
extern uint8* beginPrevLineFrame_93AC8;
extern uint8* actTexture_93AD0;
extern int pitchViewPort_93AD4;
extern uint16 widthViewPort_93AD8;
extern uint16 heightViewPort_93ADC;
extern uint8 byte_967E1;
typedef struct {
	char fog_B7934_B7924[0x4000];
	char byte_BB934_BB924[0x10000];
	char byte_CB934_CB924[0x300];
	uint8 byte_CBC34_CBC24[0x300];
} TypeStrPal; // identical to the definition in sub_main.cpp
extern TypeStrPal strPal;
void DrawTriangle_729A3_72EB3(Type_RenderPoint* pnt1, Type_RenderPoint* pnt2, Type_RenderPoint* pnt3);

namespace {

constexpr int W = 320, H = 200;

// Draws the triangle with the given vertex order; returns the frame. With a constant texture and
// shade table mapping (shade << 8 | texel) -> shade, every pixel holds the interpolated shade.
std::vector<uint8_t> Draw(uint8_t mode, const Type_RenderPoint (&tri)[3], const int (&order)[3])
{
	static std::vector<uint8_t> texture(0x20000, 0x20); // texel >= 12: mode 26 does not blend with the screen
	std::vector<uint8_t> screen(W * H, 0xFF);
	for (int i = 0; i < 0x10000; i++)
		strPal.byte_BB934_BB924[i] = (char)(i >> 8);
	for (int i = 0; i < 0x4000; i++)
		strPal.fog_B7934_B7924[i] = (char)(i >> 8); // mode 26 shades through the fog table
	beginPrevLineFrame_93AC8 = screen.data();
	pitchViewPort_93AD4 = W;
	widthViewPort_93AD8 = W;
	heightViewPort_93ADC = H;
	actTexture_93AD0 = texture.data();
	byte_967E1 = mode;
	Type_RenderPoint p[3] = { tri[order[0]], tri[order[1]], tri[order[2]] };
	DrawTriangle_729A3_72EB3(&p[0], &p[1], &p[2]);
	return screen;
}

Type_RenderPoint Vertex(int x, int y)
{
	Type_RenderPoint v{};
	v.x_0 = x;
	v.y_1 = y;
	v.z_4 = (20 << 16) + x * 2048 + y * 4096; // affine shading: 20 + x/32 + y/16
	return v;
}

void ExpectOrderIndependent(uint8_t mode, const Type_RenderPoint (&tri)[3])
{
	// Clockwise orders only: the rasteriser culls by winding, like the terrain renderer expects.
	const int orders[3][3] = { { 0, 1, 2 }, { 1, 2, 0 }, { 2, 0, 1 } };
	const auto ref = Draw(mode, tri, orders[0]);
	int drawn = 0;
	for (uint8_t px : ref)
		drawn += px != 0xFF;
	ASSERT_GT(drawn, 100) << "mode " << (int)mode << ": nothing drawn - wrong winding or clipping setup?";
	for (const auto& o : orders) {
		const auto img = Draw(mode, tri, o);
		int worst = 0, where = -1;
		for (int i = 0; i < W * H; i++) {
			if ((ref[i] == 0xFF) != (img[i] == 0xFF))
				continue; // edge pixels may differ by one
			const int d = std::abs((int)ref[i] - (int)img[i]);
			if (d > worst) {
				worst = d;
				where = i;
			}
		}
		EXPECT_LE(worst, 1) << "mode " << (int)mode << " order " << o[0] << o[1] << o[2] << ": shade differs by "
			<< worst << " at (" << where % W << "," << where / W << ")";
	}
}

} // namespace

TEST(Rasteriser, ShadingIndependentOfVertexOrder)
{
	const Type_RenderPoint a[3] = { Vertex(40, 20), Vertex(200, 60), Vertex(90, 170) };   // middle vertex right
	const Type_RenderPoint b[3] = { Vertex(150, 20), Vertex(220, 170), Vertex(30, 110) }; // middle vertex left
	for (uint8_t mode : { (uint8_t)5, (uint8_t)26 }) {
		ExpectOrderIndependent(mode, a);
		ExpectOrderIndependent(mode, b);
	}
}
