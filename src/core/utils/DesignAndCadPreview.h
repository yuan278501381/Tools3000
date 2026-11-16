#pragma once
#include "core/utils/Export.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace tools3000::core {

struct DesignImagePreviewResult {
    bool ok = false;
    bool isSnapshot = false;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t layerCount = 0;
    std::string formatLabel;
    std::string metaSummary;
    std::string dataUri;
    std::vector<std::string> pageDataUris;
};

struct DxfLayerInfo {
    std::string name;
    std::string colorHex;
    bool adaptiveDefaultColor = true;
    uint32_t entityCount = 0;
};

enum class DxfEntityKind : uint8_t {
    Line = 0,
    Polyline = 1,
    Circle = 2,
    Arc = 3,
    Text = 4
};

struct DxfEntityPrimitive {
    DxfEntityKind kind = DxfEntityKind::Line;
    std::string layer;
    std::string colorHex;
    bool adaptiveDefaultColor = true;
    bool closed = false;
    std::vector<double> coords;
    double radius = 0.0;
    double startAngleDeg = 0.0;
    double endAngleDeg = 0.0;
    double textHeight = 0.0;
    double rotationDeg = 0.0;
    std::string text;
};

struct DxfVectorPreviewResult {
    bool ok = false;
    bool truncated = false;
    std::string encoding;
    uint32_t totalEntities = 0;
    std::array<double, 4> bounds = {0.0, 0.0, 0.0, 0.0};
    std::vector<DxfLayerInfo> layers;
    std::vector<DxfEntityPrimitive> entities;
};

struct Mesh3dPreviewResult {
    bool ok = false;
    bool truncated = false;
    bool isBinary = false;
    std::string formatLabel;
    uint32_t totalTriangles = 0;
    uint32_t sampledTriangles = 0;
    std::array<double, 3> dimensions = {0.0, 0.0, 0.0};
    std::vector<float> positions;
};

class TOOLS3000CORE_API DesignAndCadPreview {
public:
    static DesignImagePreviewResult extractPsdPreview(
        const std::wstring& filePath,
        uint32_t maxDimensionPx = 2560,
        bool allowShellFallback = true);

    static DesignImagePreviewResult extractCdrPreview(
        const std::wstring& filePath,
        bool allowShellFallback = true);

    static DesignImagePreviewResult extractCadOr3dSnapshotPreview(
        const std::wstring& filePath,
        const std::string& lowerExt,
        bool allowShellFallback = true);

    static DesignImagePreviewResult extractShellThumbnailPng(
        const std::wstring& filePath,
        uint32_t requestedSizePx = 1600,
        uint32_t timeoutMs = 2500);

    static DxfVectorPreviewResult extractDxfVectorPreview(
        const std::wstring& filePath,
        size_t maxEntities = 30000);

    static Mesh3dPreviewResult extract3dMeshPreview(
        const std::wstring& filePath,
        const std::string& lowerExt,
        size_t maxTriangles = 50000);
};

} // namespace tools3000::core
