#include <gtest/gtest.h>

#include "omegaWTK/Media/ImgCodec.h"

#include "Fixtures.h"

using namespace OmegaWTK;
using namespace OmegaWTK::Media;
using namespace OmegaWTKTests;

namespace {

    BitmapImage *valueOf(StatusWithObj<BitmapImage> &res){
        EXPECT_TRUE(static_cast<bool>(res)) << res.getError();
        auto handle = res.getValue();
        return handle.get();
    }

}

TEST(LoadImageFromBuffer, DecodesPng2x2RGBA){
    auto *bytes = const_cast<ImgByte *>(static_cast<const ImgByte *>(kPng2x2RGBA));
    auto res = loadImageFromBuffer(bytes, kPng2x2RGBASize, BitmapImage::PNG);
    auto *img = valueOf(res);
    ASSERT_NE(img, nullptr);
    EXPECT_EQ(img->header.width, 2u);
    EXPECT_EQ(img->header.height, 2u);
    EXPECT_EQ(img->header.bitDepth, 8);
    EXPECT_EQ(img->header.channels, 4);
    EXPECT_EQ(img->header.color_format, BitmapImage::ColorFormat::RGBA);
    EXPECT_EQ(img->header.alpha_format, BitmapImage::AlphaFormat::Straight);
    EXPECT_EQ(img->header.stride, 8u);
    EXPECT_NE(img->data, nullptr);
}

TEST(LoadImageFromBuffer, DecodesJpeg2x2){
    auto *bytes = const_cast<ImgByte *>(static_cast<const ImgByte *>(kJpeg2x2));
    auto res = loadImageFromBuffer(bytes, kJpeg2x2Size, BitmapImage::JPEG);
    auto *img = valueOf(res);
    ASSERT_NE(img, nullptr);
    EXPECT_EQ(img->header.width, 2u);
    EXPECT_EQ(img->header.height, 2u);
    EXPECT_EQ(img->header.bitDepth, 8);
    EXPECT_EQ(img->header.channels, 4);
    EXPECT_EQ(img->header.color_format, BitmapImage::ColorFormat::RGBA);
    EXPECT_EQ(img->header.stride, 8u);
    EXPECT_NE(img->data, nullptr);
}

TEST(LoadImageFromBuffer, DecodesTiff2x2RGBA){
    auto *bytes = const_cast<ImgByte *>(static_cast<const ImgByte *>(kTiff2x2RGBA));
    auto res = loadImageFromBuffer(bytes, kTiff2x2RGBASize, BitmapImage::TIFF);
    auto *img = valueOf(res);
    ASSERT_NE(img, nullptr);
    EXPECT_EQ(img->header.width, 2u);
    EXPECT_EQ(img->header.height, 2u);
    EXPECT_EQ(img->header.bitDepth, 8);
    EXPECT_EQ(img->header.channels, 4);
    EXPECT_EQ(img->header.color_format, BitmapImage::ColorFormat::RGBA);
    EXPECT_EQ(img->header.alpha_format, BitmapImage::AlphaFormat::Straight);
    EXPECT_EQ(img->header.stride, 8u);
    EXPECT_NE(img->data, nullptr);
}

TEST(LoadImageFromBuffer, RejectsNullPointer){
    auto res = loadImageFromBuffer(nullptr, 16, BitmapImage::PNG);
    EXPECT_FALSE(static_cast<bool>(res));
}

TEST(LoadImageFromBuffer, RejectsZeroSize){
    ImgByte byte = 0;
    auto res = loadImageFromBuffer(&byte, 0, BitmapImage::PNG);
    EXPECT_FALSE(static_cast<bool>(res));
}

TEST(LoadImageFromBuffer, RejectsCorruptPng){
    unsigned char garbage[64] = {0};
    garbage[0] = 0x89; garbage[1] = 'P'; garbage[2] = 'N'; garbage[3] = 'G';
    auto res = loadImageFromBuffer(garbage, sizeof(garbage), BitmapImage::PNG);
    EXPECT_FALSE(static_cast<bool>(res));
}

TEST(LoadImageFromBuffer, RejectsCorruptJpeg){
    unsigned char garbage[64];
    for(auto &b : garbage) b = 0xAB;
    auto res = loadImageFromBuffer(garbage, sizeof(garbage), BitmapImage::JPEG);
    EXPECT_FALSE(static_cast<bool>(res));
}

TEST(LoadImageFromBuffer, RejectsCorruptTiff){
    unsigned char garbage[64];
    for(auto &b : garbage) b = 0x00;
    auto res = loadImageFromBuffer(garbage, sizeof(garbage), BitmapImage::TIFF);
    EXPECT_FALSE(static_cast<bool>(res));
}
