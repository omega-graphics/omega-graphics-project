#include <gtest/gtest.h>

#include "omega-common/img.h"

#include "Fixtures.h"

using namespace OmegaCommon::Img;
using namespace OmegaWTKTests;
using OmegaCommon::Result;

namespace {

    const BitmapImage * valueOf(Result<BitmapImage, std::string> & res){
        EXPECT_TRUE(res.isOk()) << (res.isErr() ? res.error() : std::string{});
        return res.isOk() ? &res.value() : nullptr;
    }

}

TEST(LoadImageFromBuffer, DecodesPng2x2RGBA){
    auto *bytes = const_cast<Byte *>(static_cast<const Byte *>(kPng2x2RGBA));
    auto res = loadFromBuffer(bytes, kPng2x2RGBASize, Format::PNG);
    auto *img = valueOf(res);
    ASSERT_NE(img, nullptr);
    EXPECT_EQ(img->header.width, 2u);
    EXPECT_EQ(img->header.height, 2u);
    EXPECT_EQ(img->header.bitDepth, 8);
    EXPECT_EQ(img->header.channels, 4);
    EXPECT_EQ(img->header.color_format, ColorFormat::RGBA);
    EXPECT_EQ(img->header.alpha_format, AlphaFormat::Straight);
    EXPECT_EQ(img->header.stride, 8u);
    EXPECT_NE(img->data, nullptr);
}

TEST(LoadImageFromBuffer, DecodesJpeg2x2){
    auto *bytes = const_cast<Byte *>(static_cast<const Byte *>(kJpeg2x2));
    auto res = loadFromBuffer(bytes, kJpeg2x2Size, Format::JPEG);
    auto *img = valueOf(res);
    ASSERT_NE(img, nullptr);
    EXPECT_EQ(img->header.width, 2u);
    EXPECT_EQ(img->header.height, 2u);
    EXPECT_EQ(img->header.bitDepth, 8);
    EXPECT_EQ(img->header.channels, 4);
    EXPECT_EQ(img->header.color_format, ColorFormat::RGBA);
    EXPECT_EQ(img->header.stride, 8u);
    EXPECT_NE(img->data, nullptr);
}

TEST(LoadImageFromBuffer, DecodesTiff2x2RGBA){
    auto *bytes = const_cast<Byte *>(static_cast<const Byte *>(kTiff2x2RGBA));
    auto res = loadFromBuffer(bytes, kTiff2x2RGBASize, Format::TIFF);
    auto *img = valueOf(res);
    ASSERT_NE(img, nullptr);
    EXPECT_EQ(img->header.width, 2u);
    EXPECT_EQ(img->header.height, 2u);
    EXPECT_EQ(img->header.bitDepth, 8);
    EXPECT_EQ(img->header.channels, 4);
    EXPECT_EQ(img->header.color_format, ColorFormat::RGBA);
    EXPECT_EQ(img->header.alpha_format, AlphaFormat::Straight);
    EXPECT_EQ(img->header.stride, 8u);
    EXPECT_NE(img->data, nullptr);
}

TEST(LoadImageFromBuffer, RejectsNullPointer){
    auto res = loadFromBuffer(nullptr, 16, Format::PNG);
    EXPECT_TRUE(res.isErr());
}

TEST(LoadImageFromBuffer, RejectsZeroSize){
    Byte byte = 0;
    auto res = loadFromBuffer(&byte, 0, Format::PNG);
    EXPECT_TRUE(res.isErr());
}

TEST(LoadImageFromBuffer, RejectsCorruptPng){
    unsigned char garbage[64] = {0};
    garbage[0] = 0x89; garbage[1] = 'P'; garbage[2] = 'N'; garbage[3] = 'G';
    auto res = loadFromBuffer(garbage, sizeof(garbage), Format::PNG);
    EXPECT_TRUE(res.isErr());
}

TEST(LoadImageFromBuffer, RejectsCorruptJpeg){
    unsigned char garbage[64];
    for(auto &b : garbage) b = 0xAB;
    auto res = loadFromBuffer(garbage, sizeof(garbage), Format::JPEG);
    EXPECT_TRUE(res.isErr());
}

TEST(LoadImageFromBuffer, RejectsCorruptTiff){
    unsigned char garbage[64];
    for(auto &b : garbage) b = 0x00;
    auto res = loadFromBuffer(garbage, sizeof(garbage), Format::TIFF);
    EXPECT_TRUE(res.isErr());
}
