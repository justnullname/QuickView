#pragma once
// ============================================================================
// PluginRegistry.h - Zero-LoadLibrary Static Manifest Discovery for QVX v3
// ============================================================================
// Scans plugin directories for manifest.json using yyjson without loading any
// plugin binaries (.qvx/.dll). Guarantees < 1ms cold-boot overhead.
// ============================================================================

#include "abi/qvx_core.h"
#include "abi/qvx_processor.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace QuickView {

enum class PluginSourceKind : uint8_t {
    LocalAppDir = 0,
    LocalUserDir,
    MsixBundled
};

struct PluginLocalizedText {
    std::string lang;
    std::string name;
    std::string desc;
};

struct PluginManifestAsset {
    std::string id;
    std::string displayName;
    std::string extractSubdir;
    std::string downloadUrl;
    std::string sha256;
    uint64_t sizeBytes = 0;
    bool required = false;
};

struct PluginRecord {
    std::string id;
    std::string version;
    std::string abiVersion;
    std::string author;
    std::string entryFileName;
    std::string arch;
    std::string sandboxPref;

    std::wstring pluginDir;
    std::wstring entryFullPath;
    std::wstring manifestPath;

    PluginSourceKind sourceKind = PluginSourceKind::LocalAppDir;
    uint32_t interfaceMask = 0; // Bitmask of (1u << QVX_InterfaceID)

    // ImageProcessor metadata declared statically in manifest.json
    QVX_ProcKind processorKind = QVX_PROC_KIND_UNKNOWN;
    uint32_t processorCaps = 0;

    std::vector<PluginLocalizedText> i18n;
    std::vector<PluginManifestAsset> manifestAssets;

    [[nodiscard]] bool HasInterface(QVX_InterfaceID iface) const noexcept {
        return (interfaceMask & (1u << static_cast<uint32_t>(iface))) != 0;
    }

    [[nodiscard]] std::string ResolveName(std::string_view lang) const;
    [[nodiscard]] std::string ResolveDesc(std::string_view lang) const;
};

class PluginRegistry {
public:
    // Scan plugins directory (or customRoot for tests) without calling LoadLibrary.
    void Scan(const std::wstring& customRoot = L"");

    [[nodiscard]] const std::vector<PluginRecord>& GetAll() const noexcept { return m_records; }
    [[nodiscard]] const PluginRecord* FindById(std::string_view pluginId) const noexcept;
    [[nodiscard]] std::vector<const PluginRecord*> GetByInterface(QVX_InterfaceID iface) const;

    static int CompareVersions(std::string_view a, std::string_view b) noexcept;

private:
    std::vector<PluginRecord> m_records;
};

} // namespace QuickView
