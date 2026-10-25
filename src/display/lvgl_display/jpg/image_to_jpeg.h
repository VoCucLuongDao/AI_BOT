// image_to_jpeg.h - 图像到JPEG转换的高效编码接口
// 节省约8KB SRAM的JPEG编码实现
#pragma once
#include "sdkconfig.h"
#ifndef CONFIG_IDF_TARGET_ESP32

#include <stdint.h>
#include <stddef.h>

// esp_video's linux/ioctl.h defines _IO/_IOR/_IOW/_IOWR unconditionally, while
// lwIP's sockets.h already provides the same macros.  Whenever both end up in
// the same translation unit (e.g. lvgl_display.cc) GCC emits a "macro
// redefined" warning.  Save/restore lwIP's definitions around the include
// instead of patching the managed component.
#ifdef _IO
#pragma push_macro("_IO")
#undef _IO
#define IMAGE_TO_JPEG_RESTORE__IO 1
#endif
#ifdef _IOR
#pragma push_macro("_IOR")
#undef _IOR
#define IMAGE_TO_JPEG_RESTORE__IOR 1
#endif
#ifdef _IOW
#pragma push_macro("_IOW")
#undef _IOW
#define IMAGE_TO_JPEG_RESTORE__IOW 1
#endif
#ifdef _IOWR
#pragma push_macro("_IOWR")
#undef _IOWR
#define IMAGE_TO_JPEG_RESTORE__IOWR 1
#endif

#include <linux/videodev2.h>

#ifdef IMAGE_TO_JPEG_RESTORE__IOWR
#pragma pop_macro("_IOWR")
#undef IMAGE_TO_JPEG_RESTORE__IOWR
#endif
#ifdef IMAGE_TO_JPEG_RESTORE__IOW
#pragma pop_macro("_IOW")
#undef IMAGE_TO_JPEG_RESTORE__IOW
#endif
#ifdef IMAGE_TO_JPEG_RESTORE__IOR
#pragma pop_macro("_IOR")
#undef IMAGE_TO_JPEG_RESTORE__IOR
#endif
#ifdef IMAGE_TO_JPEG_RESTORE__IO
#pragma pop_macro("_IO")
#undef IMAGE_TO_JPEG_RESTORE__IO
#endif

typedef uint32_t v4l2_pix_fmt_t; // see linux/videodev2.h for details

#ifdef __cplusplus
extern "C" {
#endif

// JPEG输出回调函数类型
// arg: 用户自定义参数, index: 当前数据索引, data: JPEG数据块, len: 数据块长度
// 返回: 实际处理的字节数
typedef size_t (*jpg_out_cb)(void *arg, size_t index, const void *data, size_t len);

/**
 * @brief 将图像格式高效转换为JPEG
 * 
 * 这个函数使用优化的JPEG编码器进行编码，主要特点：
 * - 节省约8KB的SRAM使用（静态变量改为堆分配）
 * - 支持多种图像格式输入
 * - 高质量JPEG输出
 * 
 * @param src       源图像数据
 * @param src_len   源图像数据长度
 * @param width     图像宽度
 * @param height    图像高度  
 * @param format    图像格式 (PIXFORMAT_RGB565, PIXFORMAT_RGB888, 等)
 * @param quality   JPEG质量 (1-100)
 * @param out       输出JPEG数据指针 (需要调用者释放)
 * @param out_len   输出JPEG数据长度
 * 
 * @return true 成功, false 失败
 */
bool image_to_jpeg(uint8_t *src, size_t src_len, uint16_t width, uint16_t height, 
                   v4l2_pix_fmt_t format, uint8_t quality, uint8_t **out, size_t *out_len);

/**
 * @brief 将图像格式转换为JPEG（回调版本）
 * 
 * 使用回调函数处理JPEG输出数据，适合流式传输或分块处理：
 * - 节省约8KB的SRAM使用（静态变量改为堆分配）
 * - 支持流式输出，无需预分配大缓冲区
 * - 通过回调函数逐块处理JPEG数据
 * 
 * @param src       源图像数据
 * @param src_len   源图像数据长度
 * @param width     图像宽度
 * @param height    图像高度
 * @param format    图像格式
 * @param quality   JPEG质量 (1-100)
 * @param cb        输出回调函数
 * @param arg       传递给回调函数的用户参数
 * 
 * @return true 成功, false 失败
 */
bool image_to_jpeg_cb(uint8_t *src, size_t src_len, uint16_t width, uint16_t height, 
                      v4l2_pix_fmt_t format, uint8_t quality, jpg_out_cb cb, void *arg);

#ifdef __cplusplus
}
#endif

#endif // ndef CONFIG_IDF_TARGET_ESP32
