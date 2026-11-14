#include "picture.h"

#include <wincodec.h>

#include <algorithm>
#include <vector>

namespace hc09::editor
{
    namespace
    {
        template <typename T>
        struct Com
        {
            T* p = nullptr;
            ~Com()
            {
                if (p)
                    p->Release();
            }
            T* operator->() const { return p; }
            T** operator&() { return &p; }
            explicit operator bool() const { return p != nullptr; }
        };

        IWICImagingFactory* Factory()
        {
            static IWICImagingFactory* factory = [] {
                IWICImagingFactory* f = nullptr;
                CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
                return f;
            }();
            return factory;
        }

        // The picture's largest centred square from its top (a portrait's head is at the top), scaled to
        // `size`, as premultiplied or straight BGRA.
        bool Square(const std::filesystem::path& path, uint32_t size, bool premultiplied, std::vector<uint8_t>& pixels)
        {
            IWICImagingFactory* factory = Factory();
            if (!factory)
                return false;
            Com<IWICBitmapDecoder> decoder;
            Com<IWICBitmapFrameDecode> frame;
            Com<IWICFormatConverter> converter;
            Com<IWICBitmapClipper> clipper;
            Com<IWICBitmapScaler> scaler;
            if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) ||
                FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&converter)) ||
                FAILED(converter->Initialize(frame.p, premultiplied ? GUID_WICPixelFormat32bppPBGRA : GUID_WICPixelFormat32bppBGRA,
                    WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
                return false;
            UINT width = 0, height = 0;
            converter->GetSize(&width, &height);
            if (!width || !height)
                return false;
            const UINT side = std::min(width, height);
            const WICRect crop{ INT((width - side) / 2), 0, INT(side), INT(side) };
            if (FAILED(factory->CreateBitmapClipper(&clipper)) || FAILED(clipper->Initialize(converter.p, &crop)) ||
                FAILED(factory->CreateBitmapScaler(&scaler)) ||
                FAILED(scaler->Initialize(clipper.p, size, size, WICBitmapInterpolationModeFant)))
                return false;
            pixels.resize(size_t(size) * size * 4);
            return SUCCEEDED(scaler->CopyPixels(nullptr, size * 4, UINT(pixels.size()), pixels.data()));
        }
    }

    HBITMAP LoadPreview(const std::filesystem::path& path, int size, COLORREF background)
    {
        std::vector<uint8_t> pixels;
        if (!Square(path, uint32_t(size), true, pixels))
            return nullptr;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = size;
        info.bmiHeader.biHeight = -size;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bitmap)
            return nullptr;
        // Over the background: a static control does not blend.
        const uint8_t back[3] = { GetBValue(background), GetGValue(background), GetRValue(background) };
        uint8_t* out = static_cast<uint8_t*>(bits);
        for (size_t i = 0; i < pixels.size(); i += 4)
        {
            const uint32_t alpha = pixels[i + 3];
            for (int c = 0; c < 3; c++)
                out[i + c] = uint8_t(pixels[i + c] + back[c] * (255 - alpha) / 255);
            out[i + 3] = 255;
        }
        return bitmap;
    }

    bool ImportPicture(const std::filesystem::path& source, const std::filesystem::path& destination, int size)
    {
        std::vector<uint8_t> pixels;
        if (!Square(source, uint32_t(size), false, pixels))
            return false;
        std::error_code ec;
        std::filesystem::create_directories(destination.parent_path(), ec);
        IWICImagingFactory* factory = Factory();
        Com<IWICStream> stream;
        Com<IWICBitmapEncoder> encoder;
        Com<IWICBitmapFrameEncode> frame;
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        return SUCCEEDED(factory->CreateStream(&stream)) &&
               SUCCEEDED(stream->InitializeFromFilename(destination.c_str(), GENERIC_WRITE)) &&
               SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
               SUCCEEDED(encoder->Initialize(stream.p, WICBitmapEncoderNoCache)) &&
               SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
               SUCCEEDED(frame->SetSize(UINT(size), UINT(size))) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
               SUCCEEDED(frame->WritePixels(UINT(size), UINT(size) * 4, UINT(pixels.size()), pixels.data())) &&
               SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    }
}
