#ifndef __DRIVER_OLED_H
#define __DRIVER_OLED_H

#include <stdint.h>

/*
 *  函数名：OLED_Init
 *  功能描述：初始化OLED
 *  输入参数：无
 *  输出参数：无
 *  返回值：无
 */
void OLED_Init(void);

/*
 *  函数名：OLED_SetPosition
 *  功能描述：设置像素显示的起始页和起始列地址
 *  输入参数：page-->页地址模式下的起始页地址
 *            col-->页地址模式下的起始行地址
 *  输出参数：无
 *  返回值：无
*/
void OLED_SetPosition(uint8_t page, uint8_t col);

/*
 *  函数名：OLED_Clear
 *  功能描述：清屏函数
 *  输入参数：无
 *  输出参数：无
 *  返回值：无
*/
void OLED_Clear(void);

/*
 *  函数名：OLED_PutChar
 *  功能描述：显示一个字符
 *  输入参数：x --> x坐标(0~15)
 *            y --> y坐标(0~7)
 *            c -->   显示的字符
 *  输出参数：无
 *  返回值：无
*/
void OLED_PutChar(uint8_t x, uint8_t y, char c);


/*
 *  函数名：OLED_PrintString
 *  功能描述：显示一个字符串
 *  输入参数：x --> x坐标(0~15)
 *            y --> y坐标(0~7)
 *            str -->   显示的字符串
 *  输出参数：无
 *  返回值：打印了多少个字符
 */
int OLED_PrintString(uint8_t x, uint8_t y, const char *str);


/*
 *  函数名：OLED_ClearLine
 *  功能描述：清除一行
 *  输入参数：x - 从这里开始
 *            y - 清除这行
 *  输出参数：无
 *  返回值：无
 */
void OLED_ClearLine(uint8_t x, uint8_t y);

/*
 *  OLED_PrintHex
 *  功能描述：以16进制显示数值
 *  输入参数：x - x坐标(0~15)
 *            y - y坐标(0~7)
 *            val -   显示的数据
 *            pre -   非零时显示"0x"前缀
 *  输出参数：无
 *  返回值：打印了多少个字符
 */
int OLED_PrintHex(uint8_t x, uint8_t y, uint32_t val, uint8_t pre);

/*
 *  OLED_PrintSignedVal
 *  功能描述：以10进制显示一个数值
 *  输入参数：x --> x坐标(0~15)
 *            y --> y坐标(0~7)
 *            val -->   显示的数据
 *  输出参数：无
 *  返回值：打印了多少个字符
 */
int OLED_PrintSignedVal(uint8_t x, uint8_t y, int32_t val);

void OLED_Test(void);

#endif /* __DRIVER_OLED_H */

