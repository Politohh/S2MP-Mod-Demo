#include "pch.h"
#include "ImageLoader.hpp"
#include "Console.hpp"
#include "GameUtil.hpp"
#include "DevDef.h"
#include "Hook.hpp"
#include <wincodec.h>
#include <wrl/client.h>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>

#pragma comment(lib, "windowscodecs.lib")

#pragma pack(push, 1)
struct DDS_PIXELFORMAT {
    uint32_t size;
    uint32_t flags;
    uint32_t fourCC;
    uint32_t rgbBitCount;
    uint32_t rBitMask;
    uint32_t gBitMask;
    uint32_t bBitMask;
    uint32_t aBitMask;
};

struct DDS_HEADER {
    uint32_t size;
    uint32_t flags;
    uint32_t height;
    uint32_t width;
    uint32_t pitchOrLinearSize;
    uint32_t depth;
    uint32_t mipMapCount;
    uint32_t reserved1[11];
    DDS_PIXELFORMAT ddspf;
    uint32_t caps;
    uint32_t caps2;
    uint32_t caps3;
    uint32_t caps4;
    uint32_t reserved2;
};

struct DDS_HEADER_DXT10 {
    uint32_t dxgiFormat;
    uint32_t resourceDimension;
    uint32_t miscFlag;
    uint32_t arraySize;
    uint32_t miscFlags2;
};
#pragma pack(pop)


static uint32_t MakeFourCC(char a, char b, char c, char d) {
    return ((uint32_t)(uint8_t)a) |
        ((uint32_t)(uint8_t)b << 8) |
        ((uint32_t)(uint8_t)c << 16) |
        ((uint32_t)(uint8_t)d << 24);
}

static void BuildSafeImageName(char* outName, size_t outNameSize, const char* imageName) {
    if (!outName || !outNameSize) {
        return;
    }

    snprintf(outName, outNameSize, "%s", imageName ? imageName : "null_image");

    for (char* c = outName; *c; ++c) {
        if (*c == '<' || *c == '>' || *c == ':' || *c == '"' || *c == '/' || *c == '\\' || *c == '|' || *c == '?' || *c == '*') {
            *c = '_';
        }
    }
}

struct LoadedDDSData {
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipCount = 1;
    uint32_t arraySize = 1;
    bool isCubeMap = false;
    std::vector<std::vector<uint8_t>> subresources;
    std::vector<uint32_t> rowPitches;
    std::vector<uint32_t> slicePitches;
};

static uint64_t GetLoadedDDSByteSize(const LoadedDDSData& dds);

static bool IsBlockCompressedFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM:
    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_TYPELESS:
    case DXGI_FORMAT_BC6H_UF16:
    case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_TYPELESS:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
        return true;
    default:
        return false;
    }
}

static uint32_t GetBlockBytes(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM:
        return 8;

    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_TYPELESS:
    case DXGI_FORMAT_BC6H_UF16:
    case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_TYPELESS:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
        return 16;

    default:
        return 0;
    }
}

static uint32_t GetBitsPerPixel(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT:
        return 128;

    case DXGI_FORMAT_R32G32B32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R32G32B32_UINT:
    case DXGI_FORMAT_R32G32B32_SINT:
        return 96;

    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_SINT:
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R32G32_SINT:
        return 64;

    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UINT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_SINT:
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_SINT:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_SINT:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return 32;

    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8G8_SINT:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R16_SNORM:
    case DXGI_FORMAT_R16_SINT:
    case DXGI_FORMAT_B5G6R5_UNORM:
    case DXGI_FORMAT_B5G5R5A1_UNORM:
    case DXGI_FORMAT_B4G4R4A4_UNORM:
        return 16;

    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R8_SINT:
    case DXGI_FORMAT_A8_UNORM:
        return 8;

    default:
        return 0;
    }
}

static bool GetSurfaceInfo(uint32_t width, uint32_t height, DXGI_FORMAT format, uint32_t* rowBytes, uint32_t* rows, uint32_t* totalBytes) {
    if (!rowBytes || !rows || !totalBytes) {
        return false;
    }

    if (IsBlockCompressedFormat(format)) {
        uint32_t blockBytes = GetBlockBytes(format);

        if (!blockBytes) {
            return false;
        }

        uint32_t blocksWide = std::max(1u, (width + 3) / 4);
        uint32_t blocksHigh = std::max(1u, (height + 3) / 4);

        *rowBytes = blocksWide * blockBytes;
        *rows = blocksHigh;
        *totalBytes = (*rowBytes) * (*rows);

        return true;
    }

    uint32_t bpp = GetBitsPerPixel(format);

    if (!bpp) {
        return false;
    }

    *rowBytes = (width * bpp + 7) / 8;
    *rows = height;
    *totalBytes = (*rowBytes) * (*rows);

    return true;
}

bool ImageLoader::dumpImage(GfxImage* image) {
    if (!image || !image->name) {
        return false;
    }

    char safeName[MAX_PATH]{};
    BuildSafeImageName(safeName, sizeof(safeName), image->name);

    char filename[MAX_PATH]{};
    snprintf(filename, sizeof(filename), "S2MP-Mod/dump/images/%s.dds", safeName);

    std::error_code existsError;

    if (std::filesystem::exists(filename, existsError)) {
        return true;
    }

    if (!image->texture) {
        return false;
    }

    ID3D11Device* device = nullptr;
    image->texture->GetDevice(&device);

    if (!device) {
        DEV_PRINTF("Could not get D3D device while dumping image %s", image->name);
        return false;
    }

    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);

    if (!context) {
        DEV_PRINTF("Could not get D3D context while dumping image %s", image->name);
        device->Release();
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    image->texture->GetDesc(&desc);

    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    ID3D11Texture2D* staging = nullptr;
    HRESULT hr = device->CreateTexture2D(&stagingDesc, nullptr, &staging);

    if (FAILED(hr) || !staging) {
        DEV_PRINTF("CreateTexture2D staging failed while dumping %s hr=0x%08X", image->name, hr);
        context->Release();
        device->Release();
        return false;
    }

    context->CopyResource(staging, image->texture);

    std::filesystem::create_directories("S2MP-Mod/dump/images");

    FILE* file = nullptr;
    fopen_s(&file, filename, "wb");

    if (!file) {
        DEV_PRINTF("Failed to open image dump file %s", filename);
        staging->Release();
        context->Release();
        device->Release();
        return false;
    }

    constexpr uint32_t DDSD_CAPS = 0x1;
    constexpr uint32_t DDSD_HEIGHT = 0x2;
    constexpr uint32_t DDSD_WIDTH = 0x4;
    constexpr uint32_t DDSD_PITCH = 0x8;
    constexpr uint32_t DDSD_PIXELFORMAT = 0x1000;
    constexpr uint32_t DDSD_MIPMAPCOUNT = 0x20000;
    constexpr uint32_t DDSD_LINEARSIZE = 0x80000;
    constexpr uint32_t DDPF_FOURCC = 0x4;
    constexpr uint32_t DDSCAPS_COMPLEX = 0x8;
    constexpr uint32_t DDSCAPS_TEXTURE = 0x1000;
    constexpr uint32_t DDSCAPS_MIPMAP = 0x400000;
    constexpr uint32_t DDSCAPS2_CUBEMAP = 0x200;
    constexpr uint32_t DDSCAPS2_CUBEMAP_POSITIVEX = 0x400;
    constexpr uint32_t DDSCAPS2_CUBEMAP_NEGATIVEX = 0x800;
    constexpr uint32_t DDSCAPS2_CUBEMAP_POSITIVEY = 0x1000;
    constexpr uint32_t DDSCAPS2_CUBEMAP_NEGATIVEY = 0x2000;
    constexpr uint32_t DDSCAPS2_CUBEMAP_POSITIVEZ = 0x4000;
    constexpr uint32_t DDSCAPS2_CUBEMAP_NEGATIVEZ = 0x8000;
    constexpr uint32_t DDS_RESOURCE_MISC_TEXTURECUBE = 0x4;
    constexpr uint32_t DDS_DIMENSION_TEXTURE2D = 3;

    bool blockCompressed = IsBlockCompressedFormat(desc.Format);
    bool isCubeMap = (desc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0;

    uint32_t baseRowBytes = 0;
    uint32_t baseRows = 0;
    uint32_t baseTotalBytes = 0;

    if (!GetSurfaceInfo(desc.Width, desc.Height, desc.Format, &baseRowBytes, &baseRows, &baseTotalBytes)) {
        DEV_PRINTF("Unsupported image dump format %u for %s", desc.Format, image->name);
        fclose(file);
        staging->Release();
        context->Release();
        device->Release();
        return false;
    }

    DDS_HEADER header{};
    header.size = 124;
    header.flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT;
    header.height = desc.Height;
    header.width = desc.Width;
    header.depth = 0;
    header.mipMapCount = desc.MipLevels;
    header.ddspf.size = 32;
    header.ddspf.flags = DDPF_FOURCC;
    header.ddspf.fourCC = MakeFourCC('D', 'X', '1', '0');
    header.caps = DDSCAPS_TEXTURE;
    header.pitchOrLinearSize = blockCompressed ? baseTotalBytes : baseRowBytes;
    header.flags |= blockCompressed ? DDSD_LINEARSIZE : DDSD_PITCH;

    if (desc.MipLevels > 1) {
        header.flags |= DDSD_MIPMAPCOUNT;
        header.caps |= DDSCAPS_COMPLEX | DDSCAPS_MIPMAP;
    }

    if (isCubeMap) {
        header.caps |= DDSCAPS_COMPLEX;
        header.caps2 =
            DDSCAPS2_CUBEMAP |
            DDSCAPS2_CUBEMAP_POSITIVEX |
            DDSCAPS2_CUBEMAP_NEGATIVEX |
            DDSCAPS2_CUBEMAP_POSITIVEY |
            DDSCAPS2_CUBEMAP_NEGATIVEY |
            DDSCAPS2_CUBEMAP_POSITIVEZ |
            DDSCAPS2_CUBEMAP_NEGATIVEZ;
    }
    else if (desc.ArraySize > 1) {
        header.caps |= DDSCAPS_COMPLEX;
    }

    DDS_HEADER_DXT10 dx10Header{};
    dx10Header.dxgiFormat = static_cast<uint32_t>(desc.Format);
    dx10Header.resourceDimension = DDS_DIMENSION_TEXTURE2D;
    dx10Header.miscFlag = isCubeMap ? DDS_RESOURCE_MISC_TEXTURECUBE : 0;
    dx10Header.arraySize = isCubeMap ? std::max(1u, desc.ArraySize / 6) : desc.ArraySize;
    dx10Header.miscFlags2 = 0;

    uint32_t magic = MakeFourCC('D', 'D', 'S', ' ');
    fwrite(&magic, sizeof(magic), 1, file);
    fwrite(&header, sizeof(header), 1, file);
    fwrite(&dx10Header, sizeof(dx10Header), 1, file);

    bool success = true;

    for (uint32_t arraySlice = 0; arraySlice < desc.ArraySize && success; ++arraySlice) {
        for (uint32_t mip = 0; mip < desc.MipLevels; ++mip) {
            uint32_t subresource = D3D11CalcSubresource(mip, arraySlice, desc.MipLevels);

            D3D11_MAPPED_SUBRESOURCE mapped{};
            hr = context->Map(staging, subresource, D3D11_MAP_READ, 0, &mapped);

            if (FAILED(hr)) {
                DEV_PRINTF("Map failed while dumping %s array=%u mip=%u hr=0x%08X", image->name, arraySlice, mip, hr);
                success = false;
                break;
            }

            uint32_t mipWidth = std::max(1u, desc.Width >> mip);
            uint32_t mipHeight = std::max(1u, desc.Height >> mip);
            uint32_t rowBytes = 0;
            uint32_t rows = 0;
            uint32_t totalBytes = 0;

            if (!GetSurfaceInfo(mipWidth, mipHeight, desc.Format, &rowBytes, &rows, &totalBytes)) {
                DEV_PRINTF("Could not compute dump surface info for %s array=%u mip=%u", image->name, arraySlice, mip);
                context->Unmap(staging, subresource);
                success = false;
                break;
            }

            uint8_t* src = reinterpret_cast<uint8_t*>(mapped.pData);

            for (uint32_t row = 0; row < rows; ++row) {
                fwrite(src + row * mapped.RowPitch, 1, rowBytes, file);
            }

            context->Unmap(staging, subresource);
        }
    }

    fclose(file);
    staging->Release();
    context->Release();
    device->Release();

    if (success) {
        DEV_PRINTF("Dumped image %s to %s", image->name, filename);
    }

    return success;
}

static bool AreDXGIFormatsCopyCompatible(DXGI_FORMAT textureFormat, DXGI_FORMAT ddsFormat) {
    if (textureFormat == ddsFormat) {
        return true;
    }

    auto NormalizeFormat = [](DXGI_FORMAT format) -> DXGI_FORMAT {
        switch (format) {
        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
            return DXGI_FORMAT_BC1_UNORM;

        case DXGI_FORMAT_BC2_TYPELESS:
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
            return DXGI_FORMAT_BC2_UNORM;

        case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
            return DXGI_FORMAT_BC3_UNORM;

        case DXGI_FORMAT_BC4_TYPELESS:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:
            return DXGI_FORMAT_BC4_UNORM;

        case DXGI_FORMAT_BC5_TYPELESS:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
            return DXGI_FORMAT_BC5_UNORM;

        case DXGI_FORMAT_BC6H_TYPELESS:
        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC6H_SF16:
            return DXGI_FORMAT_BC6H_UF16;

        case DXGI_FORMAT_BC7_TYPELESS:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            return DXGI_FORMAT_BC7_UNORM;

        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_R8G8B8A8_SNORM:
        case DXGI_FORMAT_R8G8B8A8_SINT:
            return DXGI_FORMAT_R8G8B8A8_UNORM;

        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return DXGI_FORMAT_B8G8R8A8_UNORM;

        case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
            return DXGI_FORMAT_B8G8R8X8_UNORM;

        default:
            return format;
        }
        };

    return NormalizeFormat(textureFormat) == NormalizeFormat(ddsFormat);
}

static bool ReplaceGfxImageResourceFromDDS(GfxImage* image, const LoadedDDSData& dds, const char* imageName) {
    if (!image || !image->texture || !imageName) {
        return false;
    }

    if (dds.isCubeMap || dds.arraySize != 1) {
        DEV_PRINTF("Refusing size-changing replacement for non-normal image %s cube=%s array=%u", imageName, dds.isCubeMap ? "true" : "false", dds.arraySize);
        return false;
    }

    ID3D11Device* device = nullptr;
    image->texture->GetDevice(&device);

    if (!device) {
        DEV_PRINTF("Could not get device for %s", imageName);
        return false;
    }

    D3D11_TEXTURE2D_DESC oldDesc{};
    image->texture->GetDesc(&oldDesc);

    std::vector<D3D11_SUBRESOURCE_DATA> initialData;
    initialData.resize(dds.mipCount);

    for (uint32_t mip = 0; mip < dds.mipCount; ++mip) {
        uint32_t subresource = D3D11CalcSubresource(mip, 0, dds.mipCount);

        initialData[mip].pSysMem = dds.subresources[subresource].data();
        initialData[mip].SysMemPitch = dds.rowPitches[subresource];
        initialData[mip].SysMemSlicePitch = dds.slicePitches[subresource];
    }

    D3D11_TEXTURE2D_DESC newDesc = oldDesc;
    newDesc.Width = dds.width;
    newDesc.Height = dds.height;
    newDesc.MipLevels = dds.mipCount;
    newDesc.ArraySize = 1;
    newDesc.Format = dds.format;

    ID3D11Texture2D* newTexture = nullptr;
    HRESULT hr = device->CreateTexture2D(&newDesc, initialData.data(), &newTexture);

    if (FAILED(hr) || !newTexture) {
        DEV_PRINTF("CreateTexture2D replacement failed for %s hr=0x%08X", imageName, hr);
        device->Release();
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = dds.format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = dds.mipCount;

    ID3D11ShaderResourceView* newShaderView = nullptr;
    hr = device->CreateShaderResourceView(newTexture, &srvDesc, &newShaderView);

    if (FAILED(hr) || !newShaderView) {
        DEV_PRINTF("CreateShaderResourceView replacement failed for %s hr=0x%08X", imageName, hr);

        newTexture->Release();
        device->Release();
        return false;
    }

    ID3D11Texture2D* oldTexture = image->texture;
    ID3D11ShaderResourceView* oldShaderView = image->shaderView;

    image->texture = newTexture;
    image->shaderView = newShaderView;

    //keep the engines original internal format enum.
    //image->imageFormat is not the same thing as DXGI_FORMAT.
    image->w = static_cast<uint16_t>(dds.width);
    image->h = static_cast<uint16_t>(dds.height);
    image->d = 1;
    image->mipCount = static_cast<uint16_t>(dds.mipCount);
    image->levelCount = static_cast<uint8_t>(dds.mipCount);

    uint64_t totalMemory64 = GetLoadedDDSByteSize(dds);
    uint32_t totalMemory = totalMemory64 > std::numeric_limits<uint32_t>::max()
        ? std::numeric_limits<uint32_t>::max()
        : static_cast<uint32_t>(totalMemory64);

    image->cardMemory = totalMemory;

    if (oldShaderView) {
        oldShaderView->Release();
    }

    if (oldTexture) {
        oldTexture->Release();
    }

    device->Release();

    DEV_PRINTF(
        "Recreated GfxImage %s as %ux%u format=%u mips=%u bytes=%u",
        imageName,
        dds.width,
        dds.height,
        dds.format,
        dds.mipCount,
        totalMemory
    );

    return true;
}

static bool LoadDDSFromFile(const char* filename, LoadedDDSData* outDDS) {
    if (!filename || !outDDS) {
        return false;
    }

    FILE* file = nullptr;
    fopen_s(&file, filename, "rb");

    if (!file) {
        DEV_PRINTF("DDS not found: %s", filename);
        return false;
    }

    uint32_t magic = 0;
    fread(&magic, sizeof(magic), 1, file);

    if (magic != MakeFourCC('D', 'D', 'S', ' ')) {
        DEV_PRINTF("Invalid DDS magic: %s", filename);
        fclose(file);
        return false;
    }

    DDS_HEADER header{};
    fread(&header, sizeof(header), 1, file);

    if (header.size != 124 || header.ddspf.size != 32) {
        DEV_PRINTF("Invalid DDS header: %s", filename);
        fclose(file);
        return false;
    }

    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t arraySize = 1;
    bool isCubeMap = false;

    if ((header.ddspf.flags & 0x4) && header.ddspf.fourCC == MakeFourCC('D', 'X', '1', '0')) {
        DDS_HEADER_DXT10 dx10{};
        fread(&dx10, sizeof(dx10), 1, file);

        format = static_cast<DXGI_FORMAT>(dx10.dxgiFormat);
        arraySize = dx10.arraySize ? dx10.arraySize : 1;
        isCubeMap = (dx10.miscFlag & 0x4) != 0;

        if (isCubeMap) {
            arraySize *= 6;
        }
    }
    else if ((header.ddspf.flags & 0x4) && header.ddspf.fourCC == MakeFourCC('D', 'X', 'T', '1')) {
        format = DXGI_FORMAT_BC1_UNORM;
    }
    else if ((header.ddspf.flags & 0x4) && header.ddspf.fourCC == MakeFourCC('D', 'X', 'T', '3')) {
        format = DXGI_FORMAT_BC2_UNORM;
    }
    else if ((header.ddspf.flags & 0x4) && header.ddspf.fourCC == MakeFourCC('D', 'X', 'T', '5')) {
        format = DXGI_FORMAT_BC3_UNORM;
    }
    else {
        DEV_PRINTF("Unsupported DDS pixel format: %s", filename);
        fclose(file);
        return false;
    }

    uint32_t mipCount = header.mipMapCount ? header.mipMapCount : 1;

    if (!header.width || !header.height || !mipCount || !arraySize || format == DXGI_FORMAT_UNKNOWN) {
        DEV_PRINTF("Invalid DDS metadata: %s", filename);
        fclose(file);
        return false;
    }

    LoadedDDSData dds{};
    dds.format = format;
    dds.width = header.width;
    dds.height = header.height;
    dds.mipCount = mipCount;
    dds.arraySize = arraySize;
    dds.isCubeMap = isCubeMap;

    uint32_t subresourceCount = arraySize * mipCount;

    dds.subresources.resize(subresourceCount);
    dds.rowPitches.resize(subresourceCount);
    dds.slicePitches.resize(subresourceCount);

    for (uint32_t arraySlice = 0; arraySlice < arraySize; ++arraySlice) {
        for (uint32_t mip = 0; mip < mipCount; ++mip) {
            uint32_t subresource = D3D11CalcSubresource(mip, arraySlice, mipCount);

            uint32_t w = std::max(1u, header.width >> mip);
            uint32_t h = std::max(1u, header.height >> mip);

            uint32_t rowBytes = 0;
            uint32_t rows = 0;
            uint32_t totalBytes = 0;

            if (!GetSurfaceInfo(w, h, format, &rowBytes, &rows, &totalBytes)) {
                DEV_PRINTF("Could not compute DDS surface info: %s format=%u", filename, format);
                fclose(file);
                return false;
            }

            dds.subresources[subresource].resize(totalBytes);
            dds.rowPitches[subresource] = rowBytes;
            dds.slicePitches[subresource] = totalBytes;

            size_t read = fread(dds.subresources[subresource].data(), 1, totalBytes, file);

            if (read != totalBytes) {
                DEV_PRINTF("DDS ended early: %s subresource=%u read=%zu expected=%u", filename, subresource, read, totalBytes);
                fclose(file);
                return false;
            }
        }
    }

    fclose(file);

    *outDDS = std::move(dds);

    return true;
}

static void BuildSafeImagePath(char* outPath, size_t outPathSize, const char* imageName, const char* extension) {
    char safeName[MAX_PATH]{};
    BuildSafeImageName(safeName, sizeof(safeName), imageName);

    snprintf(outPath, outPathSize, "S2MP-Mod/images/%s.%s", safeName, extension);
}

static bool HasReplacementImageOnDisk(const char* imageName) {
    if (!imageName) {
        return false;
    }

    char ddsPath[MAX_PATH]{};
    char pngPath[MAX_PATH]{};

    BuildSafeImagePath(ddsPath, sizeof(ddsPath), imageName, "dds");
    BuildSafeImagePath(pngPath, sizeof(pngPath), imageName, "png");

    return std::filesystem::exists(ddsPath) || std::filesystem::exists(pngPath);
}

static uint64_t GetLoadedDDSByteSize(const LoadedDDSData& dds) {
    uint64_t total = 0;

    for (uint32_t slicePitch : dds.slicePitches) {
        total += slicePitch;
    }

    return total;
}

static void LogGfxImageAndTextureMetadata(const char* prefix, const GfxImage* image, const D3D11_TEXTURE2D_DESC& desc, const char* imageName) {
    if (!image) {
        return;
    }

    const char* name = imageName ? imageName : "<unnamed>";

    DEV_PRINTF(
        "%s GfxImage %s: imageFormat=%d w=%u h=%u d=%u mipCount=%u type=%u semantic=%u category=%u flags=0x%02X levelCount=%u cardMemory=%u texFormat=%u texWidth=%u texHeight=%u texMips=%u texArray=%u texUsage=%u texBind=0x%X texCPU=0x%X texMisc=0x%X",
        prefix ? prefix : "Texture",
        name,
        image->imageFormat,
        image->w,
        image->h,
        image->d,
        image->mipCount,
        image->type,
        image->semantic,
        image->category,
        image->flags,
        image->levelCount,
        image->cardMemory,
        desc.Format,
        desc.Width,
        desc.Height,
        desc.MipLevels,
        desc.ArraySize,
        desc.Usage,
        desc.BindFlags,
        desc.CPUAccessFlags,
        desc.MiscFlags
    );
}

static bool IsRGBA8PNGTargetFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return true;
    default:
        return false;
    }
}

static bool IsBGRA8PNGTargetFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return true;
    default:
        return false;
    }
}

static bool IsBGRX8PNGTargetFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return true;
    default:
        return false;
    }
}

static bool IsUncompressedPNGTargetFormat(DXGI_FORMAT format) {
    return IsRGBA8PNGTargetFormat(format) ||
        IsBGRA8PNGTargetFormat(format) ||
        IsBGRX8PNGTargetFormat(format);
}

static bool IsBC1PNGTargetFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
        return true;
    default:
        return false;
    }
}

static bool IsBC2PNGTargetFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
        return true;
    default:
        return false;
    }
}

static bool IsBC3PNGTargetFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
        return true;
    default:
        return false;
    }
}

static bool IsBCPNGCompressionTargetFormat(DXGI_FORMAT format) {
    return IsBC1PNGTargetFormat(format) ||
        IsBC2PNGTargetFormat(format) ||
        IsBC3PNGTargetFormat(format);
}

static uint16_t PackRGB565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(
        ((static_cast<uint16_t>(r) >> 3) << 11) |
        ((static_cast<uint16_t>(g) >> 2) << 5) |
        (static_cast<uint16_t>(b) >> 3)
    );
}

static void UnpackRGB565(uint16_t color, uint8_t* outRGB) {
    uint8_t r = static_cast<uint8_t>((color >> 11) & 0x1F);
    uint8_t g = static_cast<uint8_t>((color >> 5) & 0x3F);
    uint8_t b = static_cast<uint8_t>(color & 0x1F);

    outRGB[0] = static_cast<uint8_t>((r * 255 + 15) / 31);
    outRGB[1] = static_cast<uint8_t>((g * 255 + 31) / 63);
    outRGB[2] = static_cast<uint8_t>((b * 255 + 15) / 31);
}

static bool RGBA8BlockHasPunchthroughAlpha(const uint8_t* rgbaBlock) {
    for (uint32_t pixel = 0; pixel < 16; ++pixel) {
        if (rgbaBlock[pixel * 4 + 3] < 128) {
            return true;
        }
    }

    return false;
}

static void FindRGB565Endpoints(const uint8_t* rgbaBlock, bool ignorePunchthroughAlpha, uint16_t* outColor0, uint16_t* outColor1) {
    uint8_t minRGB[3] = { 255, 255, 255 };
    uint8_t maxRGB[3] = { 0, 0, 0 };
    bool found = false;

    for (uint32_t pass = 0; pass < 2 && !found; ++pass) {
        bool ignoreAlphaThisPass = ignorePunchthroughAlpha && pass == 0;

        for (uint32_t pixel = 0; pixel < 16; ++pixel) {
            const uint8_t* src = rgbaBlock + pixel * 4;

            if (ignoreAlphaThisPass && src[3] < 128) {
                continue;
            }

            for (uint32_t channel = 0; channel < 3; ++channel) {
                minRGB[channel] = std::min(minRGB[channel], src[channel]);
                maxRGB[channel] = std::max(maxRGB[channel], src[channel]);
            }

            found = true;
        }
    }

    if (!found) {
        minRGB[0] = minRGB[1] = minRGB[2] = 0;
        maxRGB[0] = maxRGB[1] = maxRGB[2] = 0;
    }

    *outColor0 = PackRGB565(maxRGB[0], maxRGB[1], maxRGB[2]);
    *outColor1 = PackRGB565(minRGB[0], minRGB[1], minRGB[2]);
}

static void BuildBC1Palette(uint16_t color0, uint16_t color1, uint8_t palette[4][4]) {
    UnpackRGB565(color0, palette[0]);
    UnpackRGB565(color1, palette[1]);

    palette[0][3] = 255;
    palette[1][3] = 255;

    if (color0 > color1) {
        for (uint32_t channel = 0; channel < 3; ++channel) {
            palette[2][channel] = static_cast<uint8_t>((2 * palette[0][channel] + palette[1][channel] + 1) / 3);
            palette[3][channel] = static_cast<uint8_t>((palette[0][channel] + 2 * palette[1][channel] + 1) / 3);
        }

        palette[2][3] = 255;
        palette[3][3] = 255;
    }
    else {
        for (uint32_t channel = 0; channel < 3; ++channel) {
            palette[2][channel] = static_cast<uint8_t>((palette[0][channel] + palette[1][channel]) / 2);
            palette[3][channel] = 0;
        }

        palette[2][3] = 255;
        palette[3][3] = 0;
    }
}

static uint32_t RGBDistanceSquared(const uint8_t* rgba, const uint8_t* rgb) {
    int dr = static_cast<int>(rgba[0]) - static_cast<int>(rgb[0]);
    int dg = static_cast<int>(rgba[1]) - static_cast<int>(rgb[1]);
    int db = static_cast<int>(rgba[2]) - static_cast<int>(rgb[2]);

    return static_cast<uint32_t>(dr * dr + dg * dg + db * db);
}

static void EmitBC1ColorBlock(const uint8_t* rgbaBlock, bool allowPunchthroughAlpha, uint8_t* outBlock) {
    bool hasPunchthroughAlpha = allowPunchthroughAlpha && RGBA8BlockHasPunchthroughAlpha(rgbaBlock);

    uint16_t color0 = 0;
    uint16_t color1 = 0;
    FindRGB565Endpoints(rgbaBlock, hasPunchthroughAlpha, &color0, &color1);

    if (hasPunchthroughAlpha) {
        if (color0 > color1) {
            std::swap(color0, color1);
        }
    }
    else if (color0 < color1) {
        std::swap(color0, color1);
    }

    uint8_t palette[4][4]{};
    BuildBC1Palette(color0, color1, palette);

    uint32_t indices = 0;
    uint32_t colorCount = color0 > color1 ? 4 : 3;

    for (uint32_t pixel = 0; pixel < 16; ++pixel) {
        const uint8_t* src = rgbaBlock + pixel * 4;
        uint32_t bestIndex = 0;

        if (hasPunchthroughAlpha && src[3] < 128) {
            bestIndex = 3;
        }
        else {
            uint32_t bestDistance = std::numeric_limits<uint32_t>::max();

            for (uint32_t candidate = 0; candidate < colorCount; ++candidate) {
                uint32_t distance = RGBDistanceSquared(src, palette[candidate]);

                if (distance < bestDistance) {
                    bestDistance = distance;
                    bestIndex = candidate;
                }
            }
        }

        indices |= bestIndex << (pixel * 2);
    }

    outBlock[0] = static_cast<uint8_t>(color0 & 0xFF);
    outBlock[1] = static_cast<uint8_t>((color0 >> 8) & 0xFF);
    outBlock[2] = static_cast<uint8_t>(color1 & 0xFF);
    outBlock[3] = static_cast<uint8_t>((color1 >> 8) & 0xFF);
    outBlock[4] = static_cast<uint8_t>(indices & 0xFF);
    outBlock[5] = static_cast<uint8_t>((indices >> 8) & 0xFF);
    outBlock[6] = static_cast<uint8_t>((indices >> 16) & 0xFF);
    outBlock[7] = static_cast<uint8_t>((indices >> 24) & 0xFF);
}

static void EmitBC2AlphaBlock(const uint8_t* rgbaBlock, uint8_t* outBlock) {
    uint64_t alphaBits = 0;

    for (uint32_t pixel = 0; pixel < 16; ++pixel) {
        uint8_t alpha4 = static_cast<uint8_t>((rgbaBlock[pixel * 4 + 3] * 15 + 127) / 255);
        alphaBits |= static_cast<uint64_t>(alpha4) << (pixel * 4);
    }

    for (uint32_t byteIndex = 0; byteIndex < 8; ++byteIndex) {
        outBlock[byteIndex] = static_cast<uint8_t>((alphaBits >> (byteIndex * 8)) & 0xFF);
    }
}

static void BuildBC3AlphaPalette(uint8_t alpha0, uint8_t alpha1, uint8_t* palette) {
    palette[0] = alpha0;
    palette[1] = alpha1;

    if (alpha0 > alpha1) {
        palette[2] = static_cast<uint8_t>((6 * alpha0 + alpha1 + 3) / 7);
        palette[3] = static_cast<uint8_t>((5 * alpha0 + 2 * alpha1 + 3) / 7);
        palette[4] = static_cast<uint8_t>((4 * alpha0 + 3 * alpha1 + 3) / 7);
        palette[5] = static_cast<uint8_t>((3 * alpha0 + 4 * alpha1 + 3) / 7);
        palette[6] = static_cast<uint8_t>((2 * alpha0 + 5 * alpha1 + 3) / 7);
        palette[7] = static_cast<uint8_t>((alpha0 + 6 * alpha1 + 3) / 7);
    }
    else {
        palette[2] = static_cast<uint8_t>((4 * alpha0 + alpha1 + 2) / 5);
        palette[3] = static_cast<uint8_t>((3 * alpha0 + 2 * alpha1 + 2) / 5);
        palette[4] = static_cast<uint8_t>((2 * alpha0 + 3 * alpha1 + 2) / 5);
        palette[5] = static_cast<uint8_t>((alpha0 + 4 * alpha1 + 2) / 5);
        palette[6] = 0;
        palette[7] = 255;
    }
}

static void EmitBC3AlphaBlock(const uint8_t* rgbaBlock, uint8_t* outBlock) {
    uint8_t minAlpha = 255;
    uint8_t maxAlpha = 0;

    for (uint32_t pixel = 0; pixel < 16; ++pixel) {
        uint8_t alpha = rgbaBlock[pixel * 4 + 3];
        minAlpha = std::min(minAlpha, alpha);
        maxAlpha = std::max(maxAlpha, alpha);
    }

    outBlock[0] = maxAlpha;
    outBlock[1] = minAlpha;

    uint8_t palette[8]{};
    BuildBC3AlphaPalette(maxAlpha, minAlpha, palette);

    uint64_t alphaIndices = 0;

    for (uint32_t pixel = 0; pixel < 16; ++pixel) {
        uint8_t alpha = rgbaBlock[pixel * 4 + 3];
        uint32_t bestIndex = 0;
        uint32_t bestDistance = std::numeric_limits<uint32_t>::max();

        for (uint32_t candidate = 0; candidate < 8; ++candidate) {
            uint32_t distance = alpha > palette[candidate]
                ? static_cast<uint32_t>(alpha - palette[candidate])
                : static_cast<uint32_t>(palette[candidate] - alpha);

            if (distance < bestDistance) {
                bestDistance = distance;
                bestIndex = candidate;
            }
        }

        alphaIndices |= static_cast<uint64_t>(bestIndex) << (pixel * 3);
    }

    for (uint32_t byteIndex = 0; byteIndex < 6; ++byteIndex) {
        outBlock[2 + byteIndex] = static_cast<uint8_t>((alphaIndices >> (byteIndex * 8)) & 0xFF);
    }
}

static bool CompressRGBA8ToBC(const std::vector<uint8_t>& rgbaPixels, uint32_t width, uint32_t height, DXGI_FORMAT format, std::vector<uint8_t>* outData, uint32_t* outRowPitch, uint32_t* outSlicePitch) {
    if (!outData || !outRowPitch || !outSlicePitch || !width || !height) {
        return false;
    }

    if (!IsBCPNGCompressionTargetFormat(format)) {
        return false;
    }

    uint32_t blockBytes = GetBlockBytes(format);
    uint32_t blocksWide = std::max(1u, (width + 3) / 4);
    uint32_t blocksHigh = std::max(1u, (height + 3) / 4);
    uint32_t rowPitch = blocksWide * blockBytes;
    uint32_t slicePitch = rowPitch * blocksHigh;

    outData->assign(slicePitch, 0);
    *outRowPitch = rowPitch;
    *outSlicePitch = slicePitch;

    uint8_t rgbaBlock[16 * 4]{};

    for (uint32_t blockY = 0; blockY < blocksHigh; ++blockY) {
        for (uint32_t blockX = 0; blockX < blocksWide; ++blockX) {
            for (uint32_t y = 0; y < 4; ++y) {
                for (uint32_t x = 0; x < 4; ++x) {
                    uint32_t srcX = std::min(width - 1, blockX * 4 + x);
                    uint32_t srcY = std::min(height - 1, blockY * 4 + y);
                    uint32_t srcOffset = (srcY * width + srcX) * 4;
                    uint32_t blockOffset = (y * 4 + x) * 4;

                    rgbaBlock[blockOffset + 0] = rgbaPixels[srcOffset + 0];
                    rgbaBlock[blockOffset + 1] = rgbaPixels[srcOffset + 1];
                    rgbaBlock[blockOffset + 2] = rgbaPixels[srcOffset + 2];
                    rgbaBlock[blockOffset + 3] = rgbaPixels[srcOffset + 3];
                }
            }

            uint8_t* dst = outData->data() + blockY * rowPitch + blockX * blockBytes;

            if (IsBC1PNGTargetFormat(format)) {
                EmitBC1ColorBlock(rgbaBlock, true, dst);
            }
            else if (IsBC2PNGTargetFormat(format)) {
                EmitBC2AlphaBlock(rgbaBlock, dst);
                EmitBC1ColorBlock(rgbaBlock, false, dst + 8);
            }
            else if (IsBC3PNGTargetFormat(format)) {
                EmitBC3AlphaBlock(rgbaBlock, dst);
                EmitBC1ColorBlock(rgbaBlock, false, dst + 8);
            }
        }
    }

    return true;
}

static std::vector<uint8_t> DownsampleRGBA8Mip(const std::vector<uint8_t>& srcPixels, uint32_t srcWidth, uint32_t srcHeight) {
    uint32_t dstWidth = std::max(1u, srcWidth / 2);
    uint32_t dstHeight = std::max(1u, srcHeight / 2);
    std::vector<uint8_t> dstPixels(static_cast<size_t>(dstWidth) * dstHeight * 4);

    for (uint32_t y = 0; y < dstHeight; ++y) {
        for (uint32_t x = 0; x < dstWidth; ++x) {
            uint32_t accum[4] = {};
            uint32_t count = 0;

            for (uint32_t dy = 0; dy < 2; ++dy) {
                uint32_t srcY = std::min(srcHeight - 1, y * 2 + dy);

                for (uint32_t dx = 0; dx < 2; ++dx) {
                    uint32_t srcX = std::min(srcWidth - 1, x * 2 + dx);
                    uint32_t srcOffset = (srcY * srcWidth + srcX) * 4;

                    accum[0] += srcPixels[srcOffset + 0];
                    accum[1] += srcPixels[srcOffset + 1];
                    accum[2] += srcPixels[srcOffset + 2];
                    accum[3] += srcPixels[srcOffset + 3];
                    ++count;
                }
            }

            uint32_t dstOffset = (y * dstWidth + x) * 4;
            dstPixels[dstOffset + 0] = static_cast<uint8_t>(accum[0] / count);
            dstPixels[dstOffset + 1] = static_cast<uint8_t>(accum[1] / count);
            dstPixels[dstOffset + 2] = static_cast<uint8_t>(accum[2] / count);
            dstPixels[dstOffset + 3] = static_cast<uint8_t>(accum[3] / count);
        }
    }

    return dstPixels;
}

static bool ConvertRGBA8PNGToTargetDDSData(const char* filename, const GfxImage* image, const D3D11_TEXTURE2D_DESC& targetDesc, const std::vector<uint8_t>& rgbaPixels, LoadedDDSData* outDDS, const char* imageName) {
    if (!filename || !image || !outDDS) {
        return false;
    }

    imageName = imageName ? imageName : "<unnamed>";

    if (!targetDesc.Width || !targetDesc.Height || !targetDesc.MipLevels || !targetDesc.ArraySize) {
        DEV_PRINTF("PNG replacement target is invalid for %s: format=%u %ux%u mips=%u array=%u", imageName, targetDesc.Format, targetDesc.Width, targetDesc.Height, targetDesc.MipLevels, targetDesc.ArraySize);
        return false;
    }

    if (targetDesc.ArraySize != 1) {
        DEV_PRINTF("PNG replacement rejected for %s: target texture arraySize=%u. Use DDS for array/cube replacements.", imageName, targetDesc.ArraySize);
        return false;
    }

    bool uncompressedTarget = IsUncompressedPNGTargetFormat(targetDesc.Format);
    bool compressedTarget = IsBCPNGCompressionTargetFormat(targetDesc.Format);

    if (IsBlockCompressedFormat(targetDesc.Format) && !compressedTarget) {
        DEV_PRINTF("PNG replacement rejected for %s: target DXGI format=%u is block-compressed and this build can only runtime-compress PNGs to BC1/BC2/BC3. Use a DDS replacement or add DirectXTex compression support.", imageName, targetDesc.Format);
        return false;
    }

    if (!uncompressedTarget && !compressedTarget) {
        DEV_PRINTF("PNG replacement rejected for %s: unsupported target DXGI format=%u. PNG replacement must preserve the original texture format.", imageName, targetDesc.Format);
        return false;
    }

    LoadedDDSData dds{};
    dds.format = targetDesc.Format;
    dds.width = targetDesc.Width;
    dds.height = targetDesc.Height;
    dds.mipCount = targetDesc.MipLevels;
    dds.arraySize = targetDesc.ArraySize;
    dds.isCubeMap = (targetDesc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0;

    uint32_t subresourceCount = dds.arraySize * dds.mipCount;
    dds.subresources.resize(subresourceCount);
    dds.rowPitches.resize(subresourceCount);
    dds.slicePitches.resize(subresourceCount);

    std::vector<uint8_t> mipPixels = rgbaPixels;
    uint32_t mipWidth = dds.width;
    uint32_t mipHeight = dds.height;

    for (uint32_t mip = 0; mip < dds.mipCount; ++mip) {
        uint32_t subresource = D3D11CalcSubresource(mip, 0, dds.mipCount);

        if (uncompressedTarget) {
            uint64_t rowPitch64 = static_cast<uint64_t>(mipWidth) * 4;
            uint64_t slicePitch64 = rowPitch64 * mipHeight;

            if (rowPitch64 > std::numeric_limits<uint32_t>::max() || slicePitch64 > std::numeric_limits<uint32_t>::max()) {
                DEV_PRINTF("PNG replacement rejected for %s: mip %u is too large (%ux%u)", imageName, mip, mipWidth, mipHeight);
                return false;
            }

            dds.rowPitches[subresource] = static_cast<uint32_t>(rowPitch64);
            dds.slicePitches[subresource] = static_cast<uint32_t>(slicePitch64);
            dds.subresources[subresource].resize(static_cast<size_t>(slicePitch64));

            if (IsRGBA8PNGTargetFormat(targetDesc.Format)) {
                std::memcpy(dds.subresources[subresource].data(), mipPixels.data(), static_cast<size_t>(slicePitch64));
            }
            else {
                for (uint32_t pixel = 0; pixel < mipWidth * mipHeight; ++pixel) {
                    const uint8_t* src = mipPixels.data() + pixel * 4;
                    uint8_t* dst = dds.subresources[subresource].data() + pixel * 4;

                    dst[0] = src[2];
                    dst[1] = src[1];
                    dst[2] = src[0];
                    dst[3] = IsBGRX8PNGTargetFormat(targetDesc.Format) ? 255 : src[3];
                }
            }
        }
        else if (!CompressRGBA8ToBC(mipPixels, mipWidth, mipHeight, targetDesc.Format, &dds.subresources[subresource], &dds.rowPitches[subresource], &dds.slicePitches[subresource])) {
            DEV_PRINTF("PNG replacement compression failed for %s mip=%u format=%u", imageName, mip, targetDesc.Format);
            return false;
        }

        if (mip + 1 < dds.mipCount) {
            mipPixels = DownsampleRGBA8Mip(mipPixels, mipWidth, mipHeight);
            mipWidth = std::max(1u, mipWidth / 2);
            mipHeight = std::max(1u, mipHeight / 2);
        }
    }

    DEV_PRINTF(
        "PNG replacement converted for %s from %s: format=%u %ux%u mips=%u array=%u subresources=%u bytes=%llu path=%s",
        imageName,
        filename,
        dds.format,
        dds.width,
        dds.height,
        dds.mipCount,
        dds.arraySize,
        subresourceCount,
        static_cast<unsigned long long>(GetLoadedDDSByteSize(dds)),
        compressedTarget ? "runtime BC compression" : "RGBA/BGRA upload"
    );

    *outDDS = std::move(dds);
    return true;
}

static bool LoadPNGFromFileForGfxImage(const char* filename, GfxImage* image, const D3D11_TEXTURE2D_DESC& targetDesc, LoadedDDSData* outDDS, const char* imageName) {
    if (!filename || !image || !outDDS) {
        return false;
    }

    if (!std::filesystem::exists(filename)) {
        return false;
    }

    HRESULT cohr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool didInitializeCOM = SUCCEEDED(cohr);

    if (cohr == RPC_E_CHANGED_MODE) {
        didInitializeCOM = false;
    }
    else if (FAILED(cohr)) {
        DEV_PRINTF("CoInitializeEx failed for PNG load: 0x%08X", cohr);
        return false;
    }

    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));

    if (FAILED(hr)) {
        DEV_PRINTF("CoCreateInstance IWICImagingFactory failed: 0x%08X", hr);

        if (didInitializeCOM) {
            CoUninitialize();
        }

        return false;
    }

    wchar_t widePath[MAX_PATH]{};
    if (!MultiByteToWideChar(CP_UTF8, 0, filename, -1, widePath, MAX_PATH)) {
        DEV_PRINTF("MultiByteToWideChar failed for PNG path: %s", filename);

        if (didInitializeCOM) {
            CoUninitialize();
        }

        return false;
    }

    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromFilename(
        widePath,
        nullptr,
        GENERIC_READ,
        WICDecodeMetadataCacheOnLoad,
        &decoder
    );

    if (FAILED(hr)) {
        DEV_PRINTF("CreateDecoderFromFilename failed for %s hr=0x%08X", filename, hr);

        if (didInitializeCOM) {
            CoUninitialize();
        }

        return false;
    }

    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);

    if (FAILED(hr)) {
        DEV_PRINTF("GetFrame failed for %s hr=0x%08X", filename, hr);

        if (didInitializeCOM) {
            CoUninitialize();
        }

        return false;
    }

    UINT width = 0;
    UINT height = 0;
    hr = frame->GetSize(&width, &height);

    if (FAILED(hr) || width == 0 || height == 0) {
        DEV_PRINTF("GetSize failed for %s hr=0x%08X", filename, hr);

        if (didInitializeCOM) {
            CoUninitialize();
        }

        return false;
    }

    UINT targetWidth = targetDesc.Width;
    UINT targetHeight = targetDesc.Height;

    if (!targetWidth || !targetHeight) {
        DEV_PRINTF("PNG replacement target has invalid size for %s: %ux%u", imageName ? imageName : "<unnamed>", targetWidth, targetHeight);

        if (didInitializeCOM) {
            CoUninitialize();
        }

        return false;
    }

    IWICBitmapSource* bitmapSource = frame.Get();
    Microsoft::WRL::ComPtr<IWICBitmapScaler> scaler;

    if (width != targetWidth || height != targetHeight) {
        hr = factory->CreateBitmapScaler(&scaler);

        if (FAILED(hr)) {
            DEV_PRINTF("CreateBitmapScaler failed for %s hr=0x%08X", filename, hr);

            if (didInitializeCOM) {
                CoUninitialize();
            }

            return false;
        }

        hr = scaler->Initialize(frame.Get(), targetWidth, targetHeight, WICBitmapInterpolationModeFant);

        if (FAILED(hr)) {
            DEV_PRINTF("WIC scaler Initialize failed for %s %ux%u -> %ux%u hr=0x%08X", filename, width, height, targetWidth, targetHeight, hr);

            if (didInitializeCOM) {
                CoUninitialize();
            }

            return false;
        }

        bitmapSource = scaler.Get();
    }

    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(&converter);

    if (FAILED(hr)) {
        DEV_PRINTF("CreateFormatConverter failed for %s hr=0x%08X", filename, hr);

        if (didInitializeCOM) {
            CoUninitialize();
        }

        return false;
    }

    hr = converter->Initialize(
        bitmapSource,
        GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone,
        nullptr,
        0.0,
        WICBitmapPaletteTypeCustom
    );

    if (FAILED(hr)) {
        DEV_PRINTF("WIC converter Initialize failed for %s hr=0x%08X", filename, hr);

        if (didInitializeCOM) {
            CoUninitialize();
        }

        return false;
    }

    uint64_t rowPitch64 = static_cast<uint64_t>(targetWidth) * 4;
    uint64_t slicePitch64 = rowPitch64 * targetHeight;

    if (rowPitch64 > std::numeric_limits<uint32_t>::max() || slicePitch64 > std::numeric_limits<uint32_t>::max()) {
        DEV_PRINTF("PNG replacement staging image is too large for %s: %ux%u", filename, targetWidth, targetHeight);

        if (didInitializeCOM) {
            CoUninitialize();
        }

        return false;
    }

    uint32_t rowPitch = static_cast<uint32_t>(rowPitch64);
    uint32_t slicePitch = static_cast<uint32_t>(slicePitch64);
    std::vector<uint8_t> rgbaPixels(slicePitch);

    hr = converter->CopyPixels(
        nullptr,
        rowPitch,
        slicePitch,
        rgbaPixels.data()
    );

    if (FAILED(hr)) {
        DEV_PRINTF("CopyPixels failed for %s hr=0x%08X", filename, hr);

        if (didInitializeCOM) {
            CoUninitialize();
        }

        return false;
    }

   // DEV_PRINTF(
   //     "PNG decoded for %s from %s: source=%ux%u stagedRGBA=%ux%u targetFormat=%u targetMips=%u targetArray=%u",
   //     imageName ? imageName : "<unnamed>",
   //     filename,
   //     width,
   //     height,
   //     targetWidth,
   //     targetHeight,
   //     targetDesc.Format,
   //     targetDesc.MipLevels,
   //     targetDesc.ArraySize
   // );

    bool converted = ConvertRGBA8PNGToTargetDDSData(filename, image, targetDesc, rgbaPixels, outDDS, imageName);

    if (didInitializeCOM) {
        CoUninitialize();
    }

    return converted;
}
static bool ReplaceGfxImageFromFile(GfxImage* image, const char* imageName) {
    if (!image || !image->texture || !imageName) {
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    image->texture->GetDesc(&desc);
    //LogGfxImageAndTextureMetadata("Before replacement", image, desc, imageName);

    char ddsPath[MAX_PATH]{};
    BuildSafeImagePath(ddsPath, sizeof(ddsPath), imageName, "dds");

    LoadedDDSData replacement{};

    if (std::filesystem::exists(ddsPath)) {
        if (!LoadDDSFromFile(ddsPath, &replacement)) {
            return false;
        }

        DEV_PRINTF("Using DDS replacement for %s", imageName);
    }
    else {
        char pngPath[MAX_PATH]{};
        BuildSafeImagePath(pngPath, sizeof(pngPath), imageName, "png");

        if (!std::filesystem::exists(pngPath)) {
            DEV_PRINTF("No DDS/PNG replacement found for %s", imageName);
            return false;
        }

        if (!LoadPNGFromFileForGfxImage(pngPath, image, desc, &replacement, imageName)) {
            return false;
        }

        DEV_PRINTF("Using PNG replacement for %s", imageName);
    }

    bool sameDimensions =
        desc.Width == replacement.width &&
        desc.Height == replacement.height &&
        desc.MipLevels == replacement.mipCount &&
        desc.ArraySize == replacement.arraySize;

    bool sameFormatOrCompatible = AreDXGIFormatsCopyCompatible(desc.Format, replacement.format);

    if (sameDimensions && sameFormatOrCompatible && desc.Usage != D3D11_USAGE_IMMUTABLE) {
        ID3D11Device* device = nullptr;
        image->texture->GetDevice(&device);

        if (!device) {
            return false;
        }

        ID3D11DeviceContext* context = nullptr;
        device->GetImmediateContext(&context);

        if (!context) {
            device->Release();
            return false;
        }

        for (uint32_t arraySlice = 0; arraySlice < replacement.arraySize; ++arraySlice) {
            for (uint32_t mip = 0; mip < replacement.mipCount; ++mip) {
                uint32_t subresource = D3D11CalcSubresource(mip, arraySlice, replacement.mipCount);

                context->UpdateSubresource(
                    image->texture,
                    subresource,
                    nullptr,
                    replacement.subresources[subresource].data(),
                    replacement.rowPitches[subresource],
                    replacement.slicePitches[subresource]
                );
            }
        }

        context->Release();
        device->Release();

        //DEV_PRINTF("Updated existing texture for %s", imageName);

        return true;
    }

    //DEV_PRINTF(
    //    "Recreating texture for %s. Old=%ux%u fmt=%u mips=%u array=%u New=%ux%u fmt=%u mips=%u array=%u",
    //    imageName,
    //    desc.Width,
    //    desc.Height,
    //    desc.Format,
    //    desc.MipLevels,
    //    desc.ArraySize,
    //    replacement.width,
    //    replacement.height,
    //    replacement.format,
    //    replacement.mipCount,
    //    replacement.arraySize
    //);

    return ReplaceGfxImageResourceFromDDS(image, replacement, imageName);
}

static bool ReplaceGfxImageFromFile(GfxImage* image) {
    if (!image || !image->name) {
        return false;
    }

    return ReplaceGfxImageFromFile(image, image->name);
}

bool ImageLoader::loadFromDisk(GfxImage* image) {
    if (!image || !image->name) {
        return false;
    }

    if (!HasReplacementImageOnDisk(image->name)) {
        return false;
    }

    return ReplaceGfxImageFromFile(image);
}


void ImageLoader::reloadImages() {
    Console::printf("Reloading custom images...");
    GameUtil::assetPoolTraverseHelper(ASSET_TYPE_IMAGE,
        [](XAsset* asset, const char* name) -> bool {

            if (!asset || !name) {
                return true;
            }

            if (HasReplacementImageOnDisk(name)) {
                DEV_PRINTF("Found replacement image for %s", name);

                if (ImageLoader::loadFromDisk(asset->header.image)) {
                    DEV_PRINTF("Replaced image: %s", name);
                }
                else {
                    DEV_PRINTF("Failed to replace image: %s", name);
                }
            }

            return true; //keep searching
        });
}


typedef int (*Image_ClearStreamLevelResidency_t)(GfxImage* image, uint8_t streamLevel);
static Image_ClearStreamLevelResidency_t fpImage_ClearStreamLevelResidency;

int Image_ClearStreamLevelResidency_hookfunc(GfxImage* image, uint8_t streamLevel) {
    if (Functions::_Dvar_FindVar("g_dumpImages")->current.enabled) {
        ImageLoader::dumpImage(image);
    }
    if (ImageLoader::loadFromDisk(image)) {
        //DEV_PRINTF("Stream Image Loaded %s from disk", image->name);
    }
    return fpImage_ClearStreamLevelResidency(image, streamLevel);
}

typedef void (*Load_Texture_t)(void** pixels, GfxImage* image);
static Load_Texture_t fpLoad_Texture;

void Load_Texture_hookfunc(void** pixels, GfxImage* image) {
    fpLoad_Texture(pixels, image);
    if (Functions::_Dvar_FindVar("g_dumpImages")->current.enabled) {
        ImageLoader::dumpImage(image);
    }
    if (ImageLoader::loadFromDisk(image)) {
        //DEV_PRINTF("Load_Texture Loaded %s from disk", image->name);
    }
}

void ImageLoader::init() {
    //World & Model Textures (and some UI)
    Hook::create("Image_ClearStreamLevelResidency", 0x25C030_b, &Image_ClearStreamLevelResidency_hookfunc, &fpImage_ClearStreamLevelResidency);

    //HUD & UI
    Hook::create("Load_Texture", 0x896A70_b, &Load_Texture_hookfunc, &fpLoad_Texture);
}
