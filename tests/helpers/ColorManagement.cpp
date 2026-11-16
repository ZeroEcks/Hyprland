#include <helpers/cm/ColorManagement.hpp>
#include <render/types.hpp>

#include <gtest/gtest.h>
#include <cmath>

using namespace NColorManagement;

static RGBAColor convertWithoutTonemapping(RGBAColor color, PImageDescription source, PImageDescription target, bool anchorReferenceWhite = false) {
    const Render::SCMSettings settings = {
        .sourceTF        = source->value().transferFunction,
        .targetTF        = target->value().transferFunction,
        .srcTFRange      = {.min = source->value().getTFMinLuminance(), .max = source->value().getTFMaxLuminance()},
        .dstTFRange      = {.min = target->value().getTFMinLuminance(), .max = target->value().getTFMaxLuminance()},
        // The decoding tests below deliberately keep absolute luminance unchanged.
        .srcRefLuminance = anchorReferenceWhite ? source->value().luminances.reference : 80.0f,
        .dstRefLuminance = anchorReferenceWhite ? target->value().luminances.reference : 80.0f,
        .referenceLuminanceScale = anchorReferenceWhite ? static_cast<float>(target->value().luminances.reference) / source->value().luminances.reference : 1.0f,
    };
    return convertColor(color, source, target, settings);
}

static void expectNeutralColor(const RGBAColor& color, double luminance, double alpha = 1.0) {
    EXPECT_NEAR(color.c.r, luminance, 0.0001);
    EXPECT_NEAR(color.c.g, luminance, 0.0001);
    EXPECT_NEAR(color.c.b, luminance, 0.0001);
    EXPECT_DOUBLE_EQ(color.c.a, alpha);
}

static PImageDescription nativeLinearDescription(const SImageDescription::SPCLuminances& luminances) {
    return CImageDescription::from(SImageDescription{
        .transferFunction = CM_TRANSFER_FUNCTION_EXT_LINEAR,
        .primariesNameSet = true,
        .primariesNamed   = CM_PRIMARIES_SRGB,
        .luminances       = luminances,
    });
}

TEST(ColorManagement, WindowsScRGBDecodesReferenceWhite) {
    const auto color = convertWithoutTonemapping({{.r = 2.5375, .g = 2.5375, .b = 2.5375, .a = 1.0}}, SCRGB_IMAGE_DESCRIPTION, LINEAR_NN_IMAGE_DESCRIPTION);
    expectNeutralColor(color, 203.0);
}

TEST(ColorManagement, WorkBufferPreservesScRGBEncodingWithMonitorMetadata) {
    for (const auto& luminances : {
             SImageDescription::SPCLuminances{.min = 0.2f, .max = 80, .reference = 80},
             SImageDescription::SPCLuminances{.min = 0.005f, .max = 600, .reference = 203},
             SImageDescription::SPCLuminances{.min = 0.0f, .max = 10000, .reference = 203},
         }) {
        const auto workBuffer = LINEAR_IMAGE_DESCRIPTION->with(luminances);
        EXPECT_EQ(workBuffer->value().luminances, luminances);

        const auto encoded = convertWithoutTonemapping({{.r = 203.0, .g = 203.0, .b = 203.0, .a = 1.0}}, LINEAR_NN_IMAGE_DESCRIPTION, workBuffer);
        expectNeutralColor(encoded, 2.5375);

        const auto decoded = convertWithoutTonemapping(encoded, workBuffer, LINEAR_NN_IMAGE_DESCRIPTION);
        expectNeutralColor(decoded, 203.0);
    }
}

TEST(ColorManagement, EquivalentEncodingsDoNotRequireConversion) {
    const auto workBuffer = LINEAR_IMAGE_DESCRIPTION->with({.min = 0.0f, .max = 80, .reference = 203});
    auto       native     = workBuffer->value();
    native.isWindows      = false;
    const auto source     = CImageDescription::from(native);

    ASSERT_NE(source->id(), workBuffer->id());
    EXPECT_FALSE(source->needsCM(workBuffer));
    EXPECT_FALSE(workBuffer->needsCM(source));
    EXPECT_FALSE(workBuffer->needsCM(workBuffer));
}

TEST(ColorManagement, NativeLinearReferenceWhiteUsesDeclaredRange) {
    const auto source     = nativeLinearDescription({.min = 0.0f, .max = 10000, .reference = 203});
    const auto workBuffer = LINEAR_IMAGE_DESCRIPTION->with(source->value().luminances);

    const auto encoded = convertWithoutTonemapping({{.r = 0.0203, .g = 0.0203, .b = 0.0203, .a = 1.0}}, source, workBuffer);
    expectNeutralColor(encoded, 2.5375);

    const auto decoded = convertWithoutTonemapping(encoded, workBuffer, LINEAR_NN_IMAGE_DESCRIPTION);
    expectNeutralColor(decoded, 203.0);
}

TEST(ColorManagement, MatchingMetadataWithDifferentEncodingRequiresConversion) {
    for (const auto& luminances : {
             SImageDescription::SPCLuminances{.min = 0.2f, .max = 80, .reference = 80},
             SImageDescription::SPCLuminances{.min = 0.0f, .max = 10000, .reference = 203},
         }) {
        const auto source     = nativeLinearDescription(luminances);
        const auto workBuffer = LINEAR_IMAGE_DESCRIPTION->with(luminances);

        ASSERT_EQ(source->value().luminances, workBuffer->value().luminances);
        ASSERT_EQ(source->value().transferFunction, workBuffer->value().transferFunction);
        ASSERT_EQ(source->value().getPrimaries(), workBuffer->value().getPrimaries());
        EXPECT_TRUE(source->needsCM(workBuffer));
        EXPECT_TRUE(workBuffer->needsCM(source));
    }
}

TEST(ColorManagement, NativeLinearDecodingIncludesMinimumLuminance) {
    const auto source = nativeLinearDescription({.min = 1.0f, .max = 1001, .reference = 101});
    for (const auto value : {0.0, 0.1, 1.0}) {
        const auto decoded = convertWithoutTonemapping({{.r = value, .g = value, .b = value, .a = 1.0}}, source, LINEAR_NN_IMAGE_DESCRIPTION);
        expectNeutralColor(decoded, 1.0 + value * 1000.0);
    }
}

TEST(ColorManagement, NativeLinearConversionPreservesPremultipliedAlpha) {
    const auto source = nativeLinearDescription({.min = 1.0f, .max = 1001, .reference = 101});
    for (const auto alpha : {0.0, 0.25, 0.5, 1.0}) {
        const double value   = 0.1 * alpha;
        const auto   encoded = convertWithoutTonemapping({{.r = value, .g = value, .b = value, .a = alpha}}, source, LINEAR_IMAGE_DESCRIPTION);
        expectNeutralColor(encoded, 101.0 / 80.0 * alpha, alpha);

        const auto roundTrip = convertWithoutTonemapping(encoded, LINEAR_IMAGE_DESCRIPTION, source);
        expectNeutralColor(roundTrip, value, alpha);
    }
}

TEST(ColorManagement, NativeLinearConversionPreservesExtendedRange) {
    const auto source = nativeLinearDescription({.min = 0.0f, .max = 10000, .reference = 203});
    const auto color  = convertWithoutTonemapping({{.r = -0.0001, .g = 0.0203, .b = 1.25, .a = 1.0}}, source, LINEAR_NN_IMAGE_DESCRIPTION);
    EXPECT_NEAR(color.c.r, -1.0, 0.0001);
    EXPECT_NEAR(color.c.g, 203.0, 0.0001);
    EXPECT_NEAR(color.c.b, 12500.0, 0.0001);
    EXPECT_DOUBLE_EQ(color.c.a, 1.0);
}

TEST(ColorManagement, RelativeReferenceWhitePreservesSDRNeutralRamp) {
    for (const auto reference : {80U, 120U, 203U, 400U}) {
        const auto source = nativeLinearDescription({.min = 0.0f, .max = reference, .reference = reference});
        for (const auto linear : {0.005, 0.02, 0.18, 0.5, 0.8, 1.0}) {
            const auto color = convertWithoutTonemapping({{.r = linear, .g = linear, .b = linear, .a = 1.0}}, source, DEFAULT_SRGB_IMAGE_DESCRIPTION, true);
            // sRGB's declared display range is 0.2..80 nits. Its reference white is 80.
            const double signal  = (linear * 80.0 - 0.2) / 79.8;
            const double encoded = signal <= 0.0031308 ? 12.92 * signal : 1.055 * std::pow(signal, 1.0 / 2.4) - 0.055;
            expectNeutralColor(color, encoded);
        }
    }
}

TEST(ColorManagement, RelativeReferenceWhitePreservesHeadroomAndAlpha) {
    const auto source = nativeLinearDescription({.min = 0.0f, .max = 812, .reference = 203});
    const auto target = nativeLinearDescription({.min = 0.0f, .max = 320, .reference = 80});
    for (const auto alpha : {0.0, 0.25, 0.5, 1.0}) {
        for (const auto signal : {-0.05, 0.25, 0.5, 1.25}) {
            const auto color = convertWithoutTonemapping({{.r = signal * alpha, .g = signal * alpha, .b = signal * alpha, .a = alpha}}, source, target, true);
            expectNeutralColor(color, signal * alpha, alpha);
            const auto roundTrip = convertWithoutTonemapping(color, target, source, true);
            expectNeutralColor(roundTrip, signal * alpha, alpha);
        }
    }
}

TEST(ColorManagement, RelativeReferenceWhiteDoesNotDoubleScaleWorkBuffers) {
    const auto source = nativeLinearDescription({.min = 0.0f, .max = 203, .reference = 203});
    for (const auto reference : {80U, 120U, 203U}) {
        const auto work = LINEAR_IMAGE_DESCRIPTION->with({.min = 0.0f, .max = 1000, .reference = reference});
        const auto nits = LINEAR_NN_IMAGE_DESCRIPTION->with({.min = 0.0f, .max = 1000, .reference = reference});
        for (const auto linear : {0.02, 0.18, 0.5, 1.0}) {
            const RGBAColor input = {{.r = linear, .g = linear, .b = linear, .a = 1.0}};
            const auto direct     = convertWithoutTonemapping(input, source, DEFAULT_SRGB_IMAGE_DESCRIPTION, true);
            const auto encoded    = convertWithoutTonemapping(input, source, work, true);
            const auto decoded    = convertWithoutTonemapping(encoded, work, nits, true);
            expectNeutralColor(decoded, linear * reference);
            const auto composed = convertWithoutTonemapping(decoded, nits, DEFAULT_SRGB_IMAGE_DESCRIPTION, true);
            expectNeutralColor(composed, direct.c.r);
        }
    }
}

TEST(ColorManagement, ICCConversionAnchorsReferenceWhiteAndPreservesAlpha) {
    auto targetValue             = DEFAULT_SRGB_IMAGE_DESCRIPTION->value();
    targetValue.icc.present      = true;
    targetValue.icc.lutSize      = 4;
    targetValue.icc.lutDomainMin = {-0.25F, -0.25F, -0.25F};
    // A gamma-2 device: signed-square-root grid points are -0.5, 0, 0.5, 1.
    // The device clips negative light, but the input domain must include it.
    for (size_t b = 0; b < 4; ++b) {
        for (size_t g = 0; g < 4; ++g) {
            for (size_t r = 0; r < 4; ++r) {
                for (const auto i : {r, g, b})
                    targetValue.icc.lutDataPacked.push_back(std::max(0.F, (sc<float>(i) - 1.F) * 0.5F));
            }
        }
    }
    const auto target = CImageDescription::from(targetValue);
    for (const auto reference : {80U, 203U, 400U}) {
        const auto                source   = CImageDescription::from(SImageDescription{
            .transferFunction = CM_TRANSFER_FUNCTION_EXT_LINEAR,
            .primariesNameSet = true,
            .primariesNamed   = CM_PRIMARIES_BT2020,
            .luminances       = {.min = 0.F, .max = reference * 4, .reference = reference},
        });
        const Render::SCMSettings settings = {
            .sourceTF                = CM_TRANSFER_FUNCTION_EXT_LINEAR,
            .srcTFRange              = {.min = 0.F, .max = sc<float>(reference * 4)},
            .dstTFRange              = {.min = 0.2F, .max = 80.F},
            .srcRefLuminance         = sc<float>(reference),
            .dstRefLuminance         = 80.F,
            .referenceLuminanceScale = 80.F / reference,
            .convertMatrix           = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}},
        };
        for (const auto alpha : {0.0, 0.25, 0.5, 1.0}) {
            for (const auto signal : {-0.125, 0.000625, 0.03125, 0.125, 0.25, 0.5}) {
                const auto   color  = convertColor(RGBAColor{{.r = signal * alpha, .g = signal * alpha, .b = signal * alpha, .a = alpha}}, source, target, settings);
                const double device = std::sqrt(std::clamp((signal * 320.0 - 0.2) / 79.8, 0.0, 1.0));
                expectNeutralColor(color, device * alpha, alpha);
            }
        }
    }
}
