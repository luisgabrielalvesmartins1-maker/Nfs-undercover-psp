#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Estrutura para armazenar os dados decodificados
typedef struct {
    uint8_t* data;
    int width;
    int height;
} TextureData;

// Função de decodificação PVRTC 4bpp (baseado no SDK oficial)
// Retorna 1 em sucesso, 0 em falha
int PVRTDecompressPVRTC(const void* compressedData, int do2bitMode, int xDim, int yDim, uint8_t* outResultImage) {
    // Implementação simplificada que lê apenas o nível base (mip 0)
    // Nota: Esta é uma versão reduzida; para produção, recomenda-se portar o código completo do SDK.
    // O código completo está disponível em: https://github.com/powervr-graphics/Native_SDK 
    
    // Placeholder para a lógica de decodificação real
    // O código completo deve ser inserido aqui
    return 0; 
}

// Função para downsample 2x2 (1024x1024 -> 512x512)
void downsample2x2(const uint8_t* src, uint8_t* dst, int srcWidth, int srcHeight) {
    int dstWidth = srcWidth / 2;
    int dstHeight = srcHeight / 2;
    
    for (int y = 0; y < dstHeight; y++) {
        for (int x = 0; x < dstWidth; x++) {
            // Pega os 4 pixels da fonte (2x2)
            int srcX = x * 2;
            int srcY = y * 2;
            
            uint32_t sumR = 0, sumG = 0, sumB = 0, sumA = 0;
            
            for (int dy = 0; dy < 2; dy++) {
                for (int dx = 0; dx < 2; dx++) {
                    int idx = ((srcY + dy) * srcWidth + (srcX + dx)) * 4;
                    sumR += src[idx + 0];
                    sumG += src[idx + 1];
                    sumB += src[idx + 2];
                    sumA += src[idx + 3];
                }
            }
            
            int dstIdx = (y * dstWidth + x) * 4;
            dst[dstIdx + 0] = (uint8_t)(sumR / 4);
            dst[dstIdx + 1] = (uint8_t)(sumG / 4);
            dst[dstIdx + 2] = (uint8_t)(sumB / 4);
            dst[dstIdx + 3] = (uint8_t)(sumA / 4);
        }
    }
}

// Função principal para carregar textura PVRTC e preparar para PSP
TextureData loadPVRTCTexture(const void* compressedData, int width, int height) {
    TextureData result = {0};
    
    // Alocar buffer para o nível base decodificado (RGBA8888)
    size_t baseSize = width * height * 4;
    uint8_t* decoded = (uint8_t*)malloc(baseSize);
    
    // Decodificar apenas o nível base (4bpp = 4 bits por pixel)
    // do2bitMode = 0 para PVRTC 4bpp
    if (!PVRTDecompressPVRTC(compressedData, 0, width, height, decoded)) {
        free(decoded);
        return result;
    }
    
    // Verificar se precisa de downsample (PSP suporta no máximo 512x512) [citation:4]
    if (width > 512 || height > 512) {
        int newWidth = width / 2;
        int newHeight = height / 2;
        uint8_t* downsampled = (uint8_t*)malloc(newWidth * newHeight * 4);
        
        downsample2x2(decoded, downsampled, width, height);
        free(decoded);
        
        result.data = downsampled;
        result.width = newWidth;
        result.height = newHeight;
    } else {
        result.data = decoded;
        result.width = width;
        result.height = height;
    }
    
    return result;
}
