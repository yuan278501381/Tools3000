// ─────────────────────────────────────────────────────────────────────────────
// test_industry_preview.inc — 工程制图与设计格式空格快速预览引擎单元测试套件
// 覆盖:
//   1. Illustrator (.ai / .eps) 跳过 PostScript 包装头与尾注无锁多页 PDF 光栅化
//   2. Photoshop (.psd / .psb) PackBits RLE & Raw 合成图解码、CMYK 还原与 0x040C 缩略图回退
//   3. CorelDRAW (.cdr) 新版 ZIP 多页预览与老版 RIFF DISP 裸 DIB 位图自动补齐头提取
//   4. AutoCAD / 天正 (.dwg) AC1032/AC1018 Sentinel 快照直提与 SolidWorks (.sldprt) 最大 PNG 提取
//   5. AutoCAD (.dxf) GBK 中文图层名、几何图元、凸度圆弧与 MTEXT 控制符清洗
//   6. 三维网格 (.stl Binary/ASCII & .obj 四边形/负索引) 归一化、包围盒尺寸与均匀步进采样
// ─────────────────────────────────────────────────────────────────────────────

namespace industry_preview_test {

void appendBe16(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    buf.push_back(static_cast<uint8_t>(v & 0xFFu));
}

void appendBe32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    buf.push_back(static_cast<uint8_t>(v & 0xFFu));
}

void appendLe16(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
}

void appendLe32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}

void appendFloatLe(std::vector<uint8_t>& buf, float f) {
    uint8_t raw[4]{};
    std::memcpy(raw, &f, 4);
    buf.insert(buf.end(), raw, raw + 4);
}

std::vector<uint8_t> makeTinyValidPng(uint32_t w, uint32_t h, uint8_t r, uint8_t g, uint8_t b) {
    std::vector<uint8_t> rawScan((1u + static_cast<size_t>(w) * 4u) * h, 0);
    for (uint32_t y = 0; y < h; ++y) {
        uint8_t* row = rawScan.data() + static_cast<size_t>(y) * (1u + w * 4u);
        row[0] = 0; // Filter None
        for (uint32_t x = 0; x < w; ++x) {
            row[1 + x * 4 + 0] = r;
            row[1 + x * 4 + 1] = g;
            row[1 + x * 4 + 2] = b;
            row[1 + x * 4 + 3] = 255;
        }
    }
    uLongf compLen = compressBound(static_cast<uLong>(rawScan.size()));
    std::vector<uint8_t> comp(compLen);
    compress2(comp.data(), &compLen, rawScan.data(), static_cast<uLong>(rawScan.size()), Z_BEST_SPEED);
    comp.resize(compLen);

    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    auto writeChunk = [&](const char type[4], const uint8_t* data, size_t len) {
        appendBe32(png, static_cast<uint32_t>(len));
        const size_t tPos = png.size();
        png.insert(png.end(), reinterpret_cast<const uint8_t*>(type), reinterpret_cast<const uint8_t*>(type) + 4);
        if (data && len > 0) png.insert(png.end(), data, data + len);
        uLong c = crc32(0L, Z_NULL, 0);
        c = crc32(c, png.data() + tPos, static_cast<uInt>(4 + len));
        appendBe32(png, static_cast<uint32_t>(c));
    };

    uint8_t ihdr[13]{};
    ihdr[0] = static_cast<uint8_t>((w >> 24) & 0xFFu);
    ihdr[1] = static_cast<uint8_t>((w >> 16) & 0xFFu);
    ihdr[2] = static_cast<uint8_t>((w >> 8) & 0xFFu);
    ihdr[3] = static_cast<uint8_t>(w & 0xFFu);
    ihdr[4] = static_cast<uint8_t>((h >> 24) & 0xFFu);
    ihdr[5] = static_cast<uint8_t>((h >> 16) & 0xFFu);
    ihdr[6] = static_cast<uint8_t>((h >> 8) & 0xFFu);
    ihdr[7] = static_cast<uint8_t>(h & 0xFFu);
    ihdr[8] = 8;
    ihdr[9] = 6;
    writeChunk("IHDR", ihdr, sizeof(ihdr));
    writeChunk("IDAT", comp.data(), comp.size());
    writeChunk("IEND", nullptr, 0);
    return png;
}

std::vector<uint8_t> makeBare24BitDib(uint32_t w, uint32_t h, uint8_t r, uint8_t g, uint8_t b) {
    const size_t rowStride = ((static_cast<size_t>(w) * 24u + 31u) / 32u) * 4u;
    std::vector<uint8_t> dib;
    dib.reserve(40u + rowStride * h);
    appendLe32(dib, 40u); // biSize
    appendLe32(dib, w);
    appendLe32(dib, h);
    appendLe16(dib, 1u);  // biPlanes
    appendLe16(dib, 24u); // biBitCount
    appendLe32(dib, 0u);  // BI_RGB
    appendLe32(dib, static_cast<uint32_t>(rowStride * h));
    appendLe32(dib, 2835u);
    appendLe32(dib, 2835u);
    appendLe32(dib, 0u);
    appendLe32(dib, 0u);

    std::vector<uint8_t> row(rowStride, 0);
    for (uint32_t x = 0; x < w; ++x) {
        row[x * 3 + 0] = b;
        row[x * 3 + 1] = g;
        row[x * 3 + 2] = r;
    }
    for (uint32_t y = 0; y < h; ++y) {
        dib.insert(dib.end(), row.begin(), row.end());
    }
    return dib;
}

void writeBinaryZipArchive(
    const std::filesystem::path& zipPath,
    const std::vector<std::pair<std::string, std::vector<uint8_t>>>& entries) {
    struct CdRec {
        std::string name;
        uint32_t crc;
        uint32_t compLen;
        uint32_t uncompLen;
        uint32_t localOff;
    };
    std::vector<uint8_t> out;
    std::vector<CdRec> cds;

    for (const auto& [name, data] : entries) {
        const uint32_t localOff = static_cast<uint32_t>(out.size());
        const uint32_t crc = static_cast<uint32_t>(
            crc32(0L, data.data(), static_cast<uInt>(data.size())));

        std::vector<uint8_t> compBuf(data.size() + 64u);
        z_stream zs{};
        deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
        zs.next_in = const_cast<Bytef*>(data.data());
        zs.avail_in = static_cast<uInt>(data.size());
        zs.next_out = compBuf.data();
        zs.avail_out = static_cast<uInt>(compBuf.size());
        deflate(&zs, Z_FINISH);
        const uint32_t compLen = static_cast<uint32_t>(zs.total_out);
        deflateEnd(&zs);

        appendLe32(out, 0x04034b50u);
        appendLe16(out, 20u);
        appendLe16(out, 0u);
        appendLe16(out, 8u);
        appendLe16(out, 0u);
        appendLe16(out, 0u);
        appendLe32(out, crc);
        appendLe32(out, compLen);
        appendLe32(out, static_cast<uint32_t>(data.size()));
        appendLe16(out, static_cast<uint16_t>(name.size()));
        appendLe16(out, 0u);
        out.insert(out.end(), name.begin(), name.end());
        out.insert(out.end(), compBuf.begin(), compBuf.begin() + compLen);

        cds.push_back({name, crc, compLen, static_cast<uint32_t>(data.size()), localOff});
    }

    const uint32_t cdOff = static_cast<uint32_t>(out.size());
    for (const auto& c : cds) {
        appendLe32(out, 0x02014b50u);
        appendLe16(out, 20u);
        appendLe16(out, 20u);
        appendLe16(out, 0u);
        appendLe16(out, 8u);
        appendLe16(out, 0u);
        appendLe16(out, 0u);
        appendLe32(out, c.crc);
        appendLe32(out, c.compLen);
        appendLe32(out, c.uncompLen);
        appendLe16(out, static_cast<uint16_t>(c.name.size()));
        appendLe16(out, 0u);
        appendLe16(out, 0u);
        appendLe16(out, 0u);
        appendLe16(out, 0u);
        appendLe32(out, 0u);
        appendLe32(out, c.localOff);
        out.insert(out.end(), c.name.begin(), c.name.end());
    }
    const uint32_t cdLen = static_cast<uint32_t>(out.size()) - cdOff;
    appendLe32(out, 0x06054b50u);
    appendLe16(out, 0u);
    appendLe16(out, 0u);
    appendLe16(out, static_cast<uint16_t>(cds.size()));
    appendLe16(out, static_cast<uint16_t>(cds.size()));
    appendLe32(out, cdLen);
    appendLe32(out, cdOff);
    appendLe16(out, 0u);

    std::ofstream f(zipPath, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
}

std::string buildSinglePagePdfBytes() {
    std::string pdf = "%PDF-1.5\n";
    std::vector<size_t> offsets(5, 0);
    offsets[1] = pdf.size();
    pdf += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";
    offsets[2] = pdf.size();
    pdf += "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n";
    offsets[3] = pdf.size();
    pdf += "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 300] /Contents 4 0 R >>\nendobj\n";
    const std::string body = "0.85 0.22 0.22 rg 40 40 320 220 re f\n";
    offsets[4] = pdf.size();
    pdf += "4 0 obj\n<< /Length " + std::to_string(body.size()) + " >>\nstream\n" + body + "endstream\nendobj\n";
    const size_t xrefPos = pdf.size();
    pdf += "xref\n0 5\n0000000000 65535 f \n";
    for (size_t i = 1; i <= 4; ++i) {
        char line[32]{};
        std::snprintf(line, sizeof(line), "%010zu 00000 n \n", offsets[i]);
        pdf += line;
    }
    pdf += "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n" + std::to_string(xrefPos) + "\n%%EOF\n";
    return pdf;
}

void writeAllBytes(const std::filesystem::path& p, const void* data, size_t len) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
}

} // namespace industry_preview_test

TEST(IndustryPreviewTest, ExtractsAiAndEpsEmbeddedPdfPagesZeroLock) {
    using tools3000::core::OfficeAndPdfPreview;
    std::error_code ec;
    const auto tempDir = std::filesystem::temp_directory_path() /
        (L"Tools3000_IndustryAi_" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::remove_all(tempDir, ec);
    std::filesystem::create_directories(tempDir, ec);

    // Wrap a valid PDF stream inside a PostScript / Illustrator header and trailer
    const std::string psPrefix =
        "%!PS-Adobe-3.1 EPSF-3.0\n"
        "%%Creator: Adobe Illustrator(R) 28.0\n"
        "%%BoundingBox: 0 0 400 300\n"
        "%AI9_PrivateDataBegin\n";
    const std::string pdfCore = industry_preview_test::buildSinglePagePdfBytes();
    const std::string psSuffix(1500, '%'); // Large trailing PS comment block after %%EOF
    const std::string fullEps = psPrefix + pdfCore + "\n" + psSuffix + "\n%%Trailer\n";

    const auto aiPath = tempDir / L"brand_guideline.ai";
    industry_preview_test::writeAllBytes(aiPath, fullEps.data(), fullEps.size());

    const auto aiRes = OfficeAndPdfPreview::renderAiOrEpsPreviewPages(aiPath.wstring(), 4, 800);
    EXPECT_TRUE(aiRes.ok);
    EXPECT_FALSE(aiRes.passwordProtected);
    EXPECT_EQ(aiRes.totalPageCount, 1u);
    ASSERT_EQ(aiRes.pageDataUris.size(), 1u);
    EXPECT_EQ(aiRes.pageDataUris[0].rfind("data:image/png;base64,", 0), 0u);

    // Zero file lock check: immediate removal must succeed
    EXPECT_TRUE(std::filesystem::remove(aiPath, ec));
    std::filesystem::remove_all(tempDir, ec);
}

TEST(IndustryPreviewTest, DecodesPsdPackBitsRleCompositeAndResource040CFallback) {
    using tools3000::core::DesignAndCadPreview;
    std::error_code ec;
    const auto tempDir = std::filesystem::temp_directory_path() /
        (L"Tools3000_IndustryPsd_" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::remove_all(tempDir, ec);
    std::filesystem::create_directories(tempDir, ec);

    // 1. Construct 8BPS Version 1 PSD (32x24, RGB 8-bit, 3 layers, PackBits RLE Compression = 1)
    std::vector<uint8_t> psd = {'8', 'B', 'P', 'S'};
    industry_preview_test::appendBe16(psd, 1u); // Version 1 PSD
    psd.insert(psd.end(), 6, 0);                // Reserved
    industry_preview_test::appendBe16(psd, 3u); // 3 channels (RGB)
    industry_preview_test::appendBe32(psd, 24u); // Height = 24
    industry_preview_test::appendBe32(psd, 32u); // Width = 32
    industry_preview_test::appendBe16(psd, 8u);  // 8-bit depth
    industry_preview_test::appendBe16(psd, 3u);  // ColorMode = RGB (3)

    industry_preview_test::appendBe32(psd, 0u); // Section 2: Color Mode Data empty
    industry_preview_test::appendBe32(psd, 0u); // Section 3: Image Resources empty

    // Section 4: Layer and Mask Info (6 bytes: layerInfoLen=2, layerCount=3)
    industry_preview_test::appendBe32(psd, 6u);
    industry_preview_test::appendBe32(psd, 2u);
    industry_preview_test::appendBe16(psd, 3u);

    // Section 5: Image Data (Compression = 1 PackBits RLE)
    // Each row of 32 identical pixels encodes in PackBits as 2 bytes: [hdr = -31 (0xE1), value]
    industry_preview_test::appendBe16(psd, 1u);
    for (int ch = 0; ch < 3; ++ch) {
        for (int y = 0; y < 24; ++y) {
            industry_preview_test::appendBe16(psd, 2u); // 2 compressed bytes per scanline
        }
    }
    const uint8_t channelVals[3] = {230u, 90u, 45u};
    for (int ch = 0; ch < 3; ++ch) {
        for (int y = 0; y < 24; ++y) {
            psd.push_back(static_cast<uint8_t>(static_cast<int8_t>(-31))); // Repeat 1 - (-31) = 32 times
            psd.push_back(channelVals[ch]);
        }
    }

    const auto psdPath = tempDir / L"poster_design.psd";
    industry_preview_test::writeAllBytes(psdPath, psd.data(), psd.size());

    const auto res = DesignAndCadPreview::extractPsdPreview(psdPath.wstring(), 2560, false);
    ASSERT_TRUE(res.ok);
    EXPECT_FALSE(res.isSnapshot);
    EXPECT_EQ(res.width, 32u);
    EXPECT_EQ(res.height, 24u);
    EXPECT_EQ(res.layerCount, 3u);
    EXPECT_NE(res.metaSummary.find("32×24 · RGB/8 · 3 layers"), std::string::npos);
    EXPECT_EQ(res.dataUri.rfind("data:image/png;base64,", 0), 0u);
    EXPECT_TRUE(std::filesystem::remove(psdPath, ec));

    // 2. Construct PSD with ZIP compression (2) that falls back to Section 3 Resource 0x040C RawRGB thumbnail
    std::vector<uint8_t> psdZip = {'8', 'B', 'P', 'S'};
    industry_preview_test::appendBe16(psdZip, 1u);
    psdZip.insert(psdZip.end(), 6, 0);
    industry_preview_test::appendBe16(psdZip, 3u);
    industry_preview_test::appendBe32(psdZip, 100u);
    industry_preview_test::appendBe32(psdZip, 200u);
    industry_preview_test::appendBe16(psdZip, 8u);
    industry_preview_test::appendBe16(psdZip, 3u);
    industry_preview_test::appendBe32(psdZip, 0u);

    // Build 0x040C resource block (4x4 kRawRGB = 0)
    std::vector<uint8_t> resBlk = {'8', 'B', 'I', 'M'};
    industry_preview_test::appendBe16(resBlk, 0x040Cu);
    industry_preview_test::appendBe16(resBlk, 0u); // Empty Pascal string padded to 2 bytes
    const uint32_t thumbPayloadBytes = 28u + 4u * 12u;
    industry_preview_test::appendBe32(resBlk, thumbPayloadBytes);
    industry_preview_test::appendBe32(resBlk, 0u);  // format = 0 (kRawRGB)
    industry_preview_test::appendBe32(resBlk, 4u);  // width = 4
    industry_preview_test::appendBe32(resBlk, 4u);  // height = 4
    industry_preview_test::appendBe32(resBlk, 12u); // widthBytes = 12
    industry_preview_test::appendBe32(resBlk, 48u); // totalSize
    industry_preview_test::appendBe32(resBlk, 48u); // compressedSize
    industry_preview_test::appendBe16(resBlk, 24u); // bitsPerPixel
    industry_preview_test::appendBe16(resBlk, 1u);  // planes
    resBlk.insert(resBlk.end(), 48u, 180u);

    industry_preview_test::appendBe32(psdZip, static_cast<uint32_t>(resBlk.size()));
    psdZip.insert(psdZip.end(), resBlk.begin(), resBlk.end());
    industry_preview_test::appendBe32(psdZip, 0u);  // Section 4 empty
    industry_preview_test::appendBe16(psdZip, 2u);  // Section 5: Compression = 2 (ZIP)

    const auto psdZipPath = tempDir / L"zip_predicted.psd";
    industry_preview_test::writeAllBytes(psdZipPath, psdZip.data(), psdZip.size());

    const auto zipFallbackRes = DesignAndCadPreview::extractPsdPreview(psdZipPath.wstring(), 2560, false);
    ASSERT_TRUE(zipFallbackRes.ok);
    EXPECT_TRUE(zipFallbackRes.isSnapshot);
    EXPECT_EQ(zipFallbackRes.dataUri.rfind("data:image/png;base64,", 0), 0u);

    std::filesystem::remove_all(tempDir, ec);
}

TEST(IndustryPreviewTest, ExtractsCorelDrawZipMultiPageAndLegacyRiffDispPreview) {
    using tools3000::core::DesignAndCadPreview;
    std::error_code ec;
    const auto tempDir = std::filesystem::temp_directory_path() /
        (L"Tools3000_IndustryCdr_" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::remove_all(tempDir, ec);
    std::filesystem::create_directories(tempDir, ec);

    // 1. Modern ZIP CorelDRAW (.cdr) with page1.png + page2.bmp
    const auto pngPage1 = industry_preview_test::makeTinyValidPng(64, 48, 20, 120, 220);
    const auto dibPage2 = industry_preview_test::makeBare24BitDib(64, 48, 220, 80, 40);
    const auto zipCdrPath = tempDir / L"modern_x8.cdr";
    industry_preview_test::writeBinaryZipArchive(zipCdrPath, {
        {"previews/page1.png", pngPage1},
        {"metadata/thumbnails/page2.bmp", dibPage2}
    });

    const auto zipRes = DesignAndCadPreview::extractCdrPreview(zipCdrPath.wstring(), false);
    ASSERT_TRUE(zipRes.ok);
    EXPECT_EQ(zipRes.width, 64u);
    EXPECT_EQ(zipRes.height, 48u);
    ASSERT_EQ(zipRes.pageDataUris.size(), 2u);
    EXPECT_EQ(zipRes.pageDataUris[0].rfind("data:image/png;base64,", 0), 0u);
    EXPECT_EQ(zipRes.pageDataUris[1].rfind("data:image/png;base64,", 0), 0u);

    // 2. Legacy RIFF CorelDRAW X3 ("RIFF....CDR9") with DISP chunk containing CF_DIB=8 + bare DIB
    const auto riffDib = industry_preview_test::makeBare24BitDib(40, 30, 50, 180, 90);
    std::vector<uint8_t> dispPayload;
    industry_preview_test::appendLe32(dispPayload, 8u); // CF_DIB = 8
    dispPayload.insert(dispPayload.end(), riffDib.begin(), riffDib.end());

    std::vector<uint8_t> riffFile = {'R', 'I', 'F', 'F'};
    const uint32_t riffTotalBody = static_cast<uint32_t>(4u + 8u + ((dispPayload.size() + 1u) & ~size_t(1u)));
    industry_preview_test::appendLe32(riffFile, riffTotalBody);
    riffFile.insert(riffFile.end(), {'C', 'D', 'R', '9'});
    riffFile.insert(riffFile.end(), {'D', 'I', 'S', 'P'});
    industry_preview_test::appendLe32(riffFile, static_cast<uint32_t>(dispPayload.size()));
    riffFile.insert(riffFile.end(), dispPayload.begin(), dispPayload.end());
    if (dispPayload.size() % 2u != 0) riffFile.push_back(0);

    const auto riffCdrPath = tempDir / L"legacy_v9.cdr";
    industry_preview_test::writeAllBytes(riffCdrPath, riffFile.data(), riffFile.size());

    const auto riffRes = DesignAndCadPreview::extractCdrPreview(riffCdrPath.wstring(), false);
    ASSERT_TRUE(riffRes.ok);
    EXPECT_EQ(riffRes.width, 40u);
    EXPECT_EQ(riffRes.height, 30u);
    EXPECT_EQ(riffRes.dataUri.rfind("data:image/png;base64,", 0), 0u);

    std::filesystem::remove_all(tempDir, ec);
}

TEST(IndustryPreviewTest, ExtractsDwgHeaderSnapshotsAndSolidWorksEmbeddedPng) {
    using tools3000::core::DesignAndCadPreview;
    std::error_code ec;
    const auto tempDir = std::filesystem::temp_directory_path() /
        (L"Tools3000_IndustryDwg_" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::remove_all(tempDir, ec);
    std::filesystem::create_directories(tempDir, ec);

    // 1. Construct AC1032 (AutoCAD 2018+ / TArch) DWG with Sentinel at 0x80 and PNG preview
    const auto previewPng = industry_preview_test::makeTinyValidPng(120, 90, 12, 160, 200);
    std::vector<uint8_t> dwg(0x80, 0);
    const char ver[] = "AC1032";
    std::memcpy(dwg.data(), ver, 6);
    const uint32_t seekerOffset = 0x80u;
    dwg[0x0D] = static_cast<uint8_t>(seekerOffset & 0xFFu);
    dwg[0x0E] = static_cast<uint8_t>((seekerOffset >> 8) & 0xFFu);
    dwg[0x0F] = static_cast<uint8_t>((seekerOffset >> 16) & 0xFFu);
    dwg[0x10] = static_cast<uint8_t>((seekerOffset >> 24) & 0xFFu);

    const uint8_t sentinel[16] = {
        0x1F, 0x25, 0x6D, 0x07, 0xD4, 0x36, 0x28, 0x28,
        0x9D, 0x57, 0xCA, 0x3F, 0x9D, 0x44, 0x10, 0x2B
    };
    dwg.insert(dwg.end(), std::begin(sentinel), std::end(sentinel));
    const uint32_t pngAbsStart = seekerOffset + 16u + 4u + 1u + 9u;
    industry_preview_test::appendLe32(dwg, static_cast<uint32_t>(14u + previewPng.size())); // overall size
    dwg.push_back(1u); // 1 directory entry
    dwg.push_back(3u); // Code 3 PNG preview
    industry_preview_test::appendLe32(dwg, pngAbsStart);
    industry_preview_test::appendLe32(dwg, static_cast<uint32_t>(previewPng.size()));
    dwg.insert(dwg.end(), previewPng.begin(), previewPng.end());

    const auto dwgPath = tempDir / L"tarch_elevation.dwg";
    industry_preview_test::writeAllBytes(dwgPath, dwg.data(), dwg.size());

    const auto dwgRes = DesignAndCadPreview::extractCadOr3dSnapshotPreview(dwgPath.wstring(), "dwg", false);
    ASSERT_TRUE(dwgRes.ok);
    EXPECT_TRUE(dwgRes.isSnapshot);
    EXPECT_EQ(dwgRes.formatLabel, "AutoCAD 2018+ / TArch");
    EXPECT_EQ(dwgRes.width, 120u);
    EXPECT_EQ(dwgRes.height, 90u);
    EXPECT_EQ(dwgRes.dataUri.rfind("data:image/png;base64,", 0), 0u);
    EXPECT_TRUE(std::filesystem::remove(dwgPath, ec));

    // 2. SolidWorks (.sldprt) binary stream with both small 8x8 icon and large 96x72 preview PNG
    const auto smallIcon = industry_preview_test::makeTinyValidPng(8, 8, 100, 100, 100);
    const auto hiResSnap = industry_preview_test::makeTinyValidPng(96, 72, 40, 140, 240);
    std::vector<uint8_t> sld = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
    sld.insert(sld.end(), 128, 0xAA);
    sld.insert(sld.end(), smallIcon.begin(), smallIcon.end());
    sld.insert(sld.end(), 64, 0x55);
    sld.insert(sld.end(), hiResSnap.begin(), hiResSnap.end());
    sld.insert(sld.end(), 32, 0x00);

    const auto sldPath = tempDir / L"flange_shaft.sldprt";
    industry_preview_test::writeAllBytes(sldPath, sld.data(), sld.size());

    const auto sldRes = DesignAndCadPreview::extractCadOr3dSnapshotPreview(sldPath.wstring(), "sldprt", false);
    ASSERT_TRUE(sldRes.ok);
    EXPECT_EQ(sldRes.width, 96u);
    EXPECT_EQ(sldRes.height, 72u);

    std::filesystem::remove_all(tempDir, ec);
}

TEST(IndustryPreviewTest, ParsesDxfGbkChineseLayersEntitiesAndCleansMTextFormatting) {
    using tools3000::core::DesignAndCadPreview;
    using tools3000::core::DxfEntityKind;

    std::error_code ec;
    const auto tempDir = std::filesystem::temp_directory_path() /
        (L"Tools3000_IndustryDxf_" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::remove_all(tempDir, ec);
    std::filesystem::create_directories(tempDir, ec);

    const std::wstring dxfWide =
        L"0\r\nSECTION\r\n"
        L"2\r\nTABLES\r\n"
        L"0\r\nTABLE\r\n2\r\nLAYER\r\n"
        L"0\r\nLAYER\r\n2\r\n墙体层\r\n62\r\n4\r\n"
        L"0\r\nLAYER\r\n2\r\n轴网标注\r\n62\r\n1\r\n"
        L"0\r\nENDTAB\r\n"
        L"0\r\nENDSEC\r\n"
        L"0\r\nSECTION\r\n"
        L"2\r\nENTITIES\r\n"
        L"0\r\nLINE\r\n8\r\n墙体层\r\n10\r\n0.0\r\n20\r\n0.0\r\n11\r\n600.0\r\n21\r\n0.0\r\n"
        L"0\r\nLWPOLYLINE\r\n8\r\n墙体层\r\n70\r\n1\r\n10\r\n0.0\r\n20\r\n0.0\r\n42\r\n0.4142\r\n10\r\n600.0\r\n20\r\n400.0\r\n"
        L"0\r\nCIRCLE\r\n8\r\n轴网标注\r\n10\r\n300.0\r\n20\r\n200.0\r\n40\r\n50.0\r\n"
        L"0\r\nTEXT\r\n8\r\n轴网标注\r\n10\r\n50.0\r\n20\r\n50.0\r\n40\r\n14.0\r\n1\r\n%%c25%%p0.02\r\n"
        L"0\r\nMTEXT\r\n8\r\n轴网标注\r\n10\r\n120.0\r\n20\r\n80.0\r\n40\r\n16.0\r\n1\r\n{\\fSimSun|b0|i0;承重剪力墙\\P厚240mm}\r\n"
        L"0\r\nENDSEC\r\n0\r\nEOF\r\n";

    // Encode explicitly as GBK (CP 936) to verify zero-garble Chinese decoding
    const UINT cp = IsValidCodePage(936) ? 936u : CP_UTF8;
    const int mbLen = WideCharToMultiByte(
        cp, 0, dxfWide.data(), static_cast<int>(dxfWide.size()), nullptr, 0, nullptr, nullptr);
    ASSERT_GT(mbLen, 0);
    std::string dxfBytes(static_cast<size_t>(mbLen), '\0');
    WideCharToMultiByte(
        cp, 0, dxfWide.data(), static_cast<int>(dxfWide.size()), dxfBytes.data(), mbLen, nullptr, nullptr);

    const auto dxfPath = tempDir / L"floorplan_gbk.dxf";
    industry_preview_test::writeAllBytes(dxfPath, dxfBytes.data(), dxfBytes.size());

    const auto dxfRes = DesignAndCadPreview::extractDxfVectorPreview(dxfPath.wstring(), 30000);
    ASSERT_TRUE(dxfRes.ok);
    if (cp == 936u) {
        EXPECT_EQ(dxfRes.encoding, "GBK");
    }
    ASSERT_EQ(dxfRes.layers.size(), 2u);
    EXPECT_EQ(dxfRes.layers[0].name, "\xE5\xA2\x99\xE4\xBD\x93\xE5\xB1\x82");     // UTF-8 "墙体层"
    EXPECT_EQ(dxfRes.layers[1].name, "\xE8\xBD\xB4\xE7\xBD\x91\xE6\xA0\x87\xE6\xB3\xA8"); // UTF-8 "轴网标注"

    ASSERT_EQ(dxfRes.entities.size(), 5u);
    EXPECT_EQ(dxfRes.entities[0].kind, DxfEntityKind::Line);
    EXPECT_EQ(dxfRes.entities[0].colorHex, "#06B6D4"); // Inherited ACI 4 Cyan from 墙体层
    EXPECT_EQ(dxfRes.entities[3].kind, DxfEntityKind::Text);
    EXPECT_EQ(dxfRes.entities[3].text, "\xE2\x8C\x80" "25" "\xC2\xB1" "0.02"); // "⌀25±0.02"
    EXPECT_EQ(dxfRes.entities[4].kind, DxfEntityKind::Text);
    EXPECT_EQ(dxfRes.entities[4].text, "\xE6\x89\xBF\xE9\x87\x8D\xE5\x89\xAA\xE5\x8A\x9B\xE5\xA2\x99\n\xE5\x8E\x9A" "240mm"); // "承重剪力墙\n厚240mm"

    EXPECT_LE(dxfRes.bounds[0], 0.0);
    EXPECT_GE(dxfRes.bounds[2], 600.0);

    std::filesystem::remove_all(tempDir, ec);
}

TEST(IndustryPreviewTest, ParsesBinaryAndAsciiStlAndObjMeshesWithUniformSampling) {
    using tools3000::core::DesignAndCadPreview;
    std::error_code ec;
    const auto tempDir = std::filesystem::temp_directory_path() /
        (L"Tools3000_Industry3d_" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::remove_all(tempDir, ec);
    std::filesystem::create_directories(tempDir, ec);

    // 1. Binary STL whose 80-byte header starts with "solid " trap, containing 6 triangles spanning [0..20, 0..30, 0..40]
    std::vector<uint8_t> binStl(80, 0);
    const char trapHdr[] = "solid tricky_binary_stl_export";
    std::memcpy(binStl.data(), trapHdr, sizeof(trapHdr) - 1);
    industry_preview_test::appendLe32(binStl, 6u); // 6 triangles

    auto pushBinTri = [&](float x0, float y0, float z0, float x1, float y1, float z1, float x2, float y2, float z2) {
        industry_preview_test::appendFloatLe(binStl, 0.0f);
        industry_preview_test::appendFloatLe(binStl, 0.0f);
        industry_preview_test::appendFloatLe(binStl, 1.0f);
        industry_preview_test::appendFloatLe(binStl, x0);
        industry_preview_test::appendFloatLe(binStl, y0);
        industry_preview_test::appendFloatLe(binStl, z0);
        industry_preview_test::appendFloatLe(binStl, x1);
        industry_preview_test::appendFloatLe(binStl, y1);
        industry_preview_test::appendFloatLe(binStl, z1);
        industry_preview_test::appendFloatLe(binStl, x2);
        industry_preview_test::appendFloatLe(binStl, y2);
        industry_preview_test::appendFloatLe(binStl, z2);
        industry_preview_test::appendLe16(binStl, 0u);
    };
    pushBinTri(0, 0, 0, 20, 0, 0, 20, 30, 0);
    pushBinTri(0, 0, 0, 20, 30, 0, 0, 30, 0);
    pushBinTri(0, 0, 40, 20, 0, 40, 20, 30, 40);
    pushBinTri(0, 0, 40, 20, 30, 40, 0, 30, 40);
    pushBinTri(0, 0, 0, 20, 0, 0, 20, 0, 40);
    pushBinTri(0, 0, 0, 20, 0, 40, 0, 0, 40);

    const auto binStlPath = tempDir / L"bracket_bin.stl";
    industry_preview_test::writeAllBytes(binStlPath, binStl.data(), binStl.size());

    const auto binRes = DesignAndCadPreview::extract3dMeshPreview(binStlPath.wstring(), "stl", 4);
    ASSERT_TRUE(binRes.ok);
    EXPECT_TRUE(binRes.isBinary);
    EXPECT_TRUE(binRes.truncated);
    EXPECT_EQ(binRes.totalTriangles, 6u);
    EXPECT_EQ(binRes.sampledTriangles, 4u);
    EXPECT_DOUBLE_EQ(binRes.dimensions[0], 20.0);
    EXPECT_DOUBLE_EQ(binRes.dimensions[1], 30.0);
    EXPECT_DOUBLE_EQ(binRes.dimensions[2], 40.0);
    ASSERT_EQ(binRes.positions.size(), 36u);
    for (float v : binRes.positions) {
        EXPECT_GE(v, -1.001f);
        EXPECT_LE(v, 1.001f);
    }

    // 2. ASCII STL + Wavefront OBJ with quad fan triangulation and negative indices
    const std::string asciiStl =
        "solid wedge\n"
        "  facet normal 0 0 1\n"
        "    outer loop\n"
        "      vertex 0 0 0\n"
        "      vertex 10 0 0\n"
        "      vertex 0 15 5\n"
        "    endloop\n"
        "  endfacet\n"
        "endsolid wedge\n";
    const auto asciiStlPath = tempDir / L"wedge_ascii.stl";
    industry_preview_test::writeAllBytes(asciiStlPath, asciiStl.data(), asciiStl.size());

    const auto asciiRes = DesignAndCadPreview::extract3dMeshPreview(asciiStlPath.wstring(), "stl", 50000);
    ASSERT_TRUE(asciiRes.ok);
    EXPECT_FALSE(asciiRes.isBinary);
    EXPECT_EQ(asciiRes.totalTriangles, 1u);
    EXPECT_DOUBLE_EQ(asciiRes.dimensions[0], 10.0);
    EXPECT_DOUBLE_EQ(asciiRes.dimensions[1], 15.0);
    EXPECT_DOUBLE_EQ(asciiRes.dimensions[2], 5.0);

    const std::string objContent =
        "# Quad plate\n"
        "v 0.0 0.0 0.0\n"
        "v 50.0 0.0 0.0\n"
        "v 50.0 25.0 0.0\n"
        "v 0.0 25.0 10.0\n"
        "f -4/1/1 -3/2/1 -2/3/1 -1/4/1\n";
    const auto objPath = tempDir / L"plate.obj";
    industry_preview_test::writeAllBytes(objPath, objContent.data(), objContent.size());

    const auto objRes = DesignAndCadPreview::extract3dMeshPreview(objPath.wstring(), "obj", 50000);
    ASSERT_TRUE(objRes.ok);
    EXPECT_EQ(objRes.totalTriangles, 2u); // Quad triangulated into 2 triangles
    EXPECT_DOUBLE_EQ(objRes.dimensions[0], 50.0);
    EXPECT_DOUBLE_EQ(objRes.dimensions[1], 25.0);
    EXPECT_DOUBLE_EQ(objRes.dimensions[2], 10.0);

    std::filesystem::remove_all(tempDir, ec);
}

TEST(IndustryPreviewTest, DecodesPsbVersion2Cmyk16BitAndShellThumbnailZeroGdiLeak) {
    using tools3000::core::DesignAndCadPreview;
    std::error_code ec;
    const auto tempDir = std::filesystem::temp_directory_path() /
        (L"Tools3000_IndustryPsb_" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::remove_all(tempDir, ec);
    std::filesystem::create_directories(tempDir, ec);

    // Construct Version 2 PSB (16x12, CMYK=4, 16-bit depth, Raw Uncompressed = 0)
    std::vector<uint8_t> psb = {'8', 'B', 'P', 'S'};
    industry_preview_test::appendBe16(psb, 2u);  // Version 2 PSB
    psb.insert(psb.end(), 6, 0);
    industry_preview_test::appendBe16(psb, 4u);  // 4 channels (CMYK)
    industry_preview_test::appendBe32(psb, 12u); // Height = 12
    industry_preview_test::appendBe32(psb, 16u); // Width = 16
    industry_preview_test::appendBe16(psb, 16u); // 16-bit depth
    industry_preview_test::appendBe16(psb, 4u);  // ColorMode = CMYK (4)

    industry_preview_test::appendBe32(psb, 0u);  // Section 2 empty
    industry_preview_test::appendBe32(psb, 0u);  // Section 3 empty
    // Section 4: PSB uses 8-byte length!
    industry_preview_test::appendBe32(psb, 0u);
    industry_preview_test::appendBe32(psb, 0u);

    // Section 5: Compression = 0 (Raw), 4 channels * 12 rows * 16 cols * 2 bytes
    industry_preview_test::appendBe16(psb, 0u);
    const uint16_t chSamples[4] = {0x1000u, 0x8000u, 0xFF00u, 0xE000u};
    for (int ch = 0; ch < 4; ++ch) {
        for (int i = 0; i < 12 * 16; ++i) {
            industry_preview_test::appendBe16(psb, chSamples[ch]);
        }
    }

    const auto psbPath = tempDir / L"print_master.psb";
    industry_preview_test::writeAllBytes(psbPath, psb.data(), psb.size());

    // Downsample maxDimensionPx capped at 256 in engine, verify formatLabel == PSB and CMYK/16 metadata
    const auto psbRes = DesignAndCadPreview::extractPsdPreview(psbPath.wstring(), 1024, false);
    ASSERT_TRUE(psbRes.ok);
    EXPECT_EQ(psbRes.formatLabel, "PSB");
    EXPECT_NE(psbRes.metaSummary.find("16×12 · CMYK/16"), std::string::npos);
    EXPECT_EQ(psbRes.dataUri.rfind("data:image/png;base64,", 0), 0u);

    // Verify extractShellThumbnailPng gracefully handles non-existent files & real PNGs with zero GDI handle leak
    const auto realPngBytes = industry_preview_test::makeTinyValidPng(64, 64, 30, 144, 255);
    const auto realPngPath = tempDir / L"shell_probe.png";
    industry_preview_test::writeAllBytes(realPngPath, realPngBytes.data(), realPngBytes.size());

    // Warm up COM / Shell DLLs once before measuring steady-state GDI object count
    (void)DesignAndCadPreview::extractShellThumbnailPng((tempDir / L"missing.dwg").wstring(), 256, 1500);
    const DWORD gdiBefore = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    for (int iter = 0; iter < 3; ++iter) {
        (void)DesignAndCadPreview::extractShellThumbnailPng(realPngPath.wstring(), 256, 1500);
    }
    const DWORD gdiAfter = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    EXPECT_LE(gdiAfter, gdiBefore + 2u);

    std::filesystem::remove_all(tempDir, ec);
}
